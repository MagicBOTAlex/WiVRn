{
  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  inputs.flake-parts.url = "github:hercules-ci/flake-parts";

  outputs =
    inputs@{ nixpkgs, flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [ "x86_64-linux" ];
      perSystem =
        { lib, pkgs, ... }:
        let
          # Extra build inputs only inside the devShell.
          devBuildInputs = [
            # for client build
            pkgs.curl
            pkgs.ktx-tools
          ];
          # Tools used for development work (clangd, clang-format)
          devTools = [
            pkgs.clang-tools
            pkgs.gdb
            pkgs.ktx-tools
          ];
          # Extra build inputs not yet in the nixpkgs recipe.
          extraBuildInputs = [
            # XRT_FEATURE_DEBUG_GUI requires SDL2
            pkgs.sdl2-compat

            pkgs.libpng
            pkgs.kdePackages.kirigami-addons
          ];
          extraNativeBuildInputs = [
            pkgs.ninja
            pkgs.util-linux
          ];

          # Release build: the nixpkgs default build type. enableDebugging (the
          # old wrapper) forces -Og, which is ~2.4x slower on the per-pixel hot
          # paths and makes the packaged server unusable for streaming.
          # `packages.debug` still provides the debug build when needed.
          package =
            pkgs.wivrn.overrideAttrs (
              finalAttrs: oldAttrs: {
                # Filter the directories and files we don't need to keep to avoid needless rebuilds
                src = lib.cleanSourceWith {
                  filter =
                    name: type:
                    let
                      baseName = baseNameOf (toString name);
                    in
                    (lib.cleanSourceFilter name type)
                    && !(
                      (
                        type == "directory"
                        && (baseName == ".direnv" || baseName == ".cxx" || lib.hasPrefix "build" baseName)
                      )
                      || baseName == ".envrc"
                    );
                  src = ./.;
                };
                version = "next";

                # Because src is just a folder path and not a set from a fetcher, it doesn't need to be unpacked, so having a postUnpack throws an error.
                # We also don't need the check since we read the revision from the monado-rev file.
                postUnpack = null;

                separateDebugInfo = false;

                monado = pkgs.applyPatches {
                  inherit (oldAttrs.monado) patches postPatch;
                  # Force a refetch when the monado rev changes.
                  src = pkgs.testers.invalidateFetcherByDrvHash pkgs.fetchFromGitLab {
                    inherit (oldAttrs.monado.src) owner repo;
                    domain = "gitlab.freedesktop.org";
                    # Keep in sync with CMakeLists.txt monado rev
                    rev = lib.strings.trim (builtins.readFile ./monado-rev);
                    # Nix will output the correct hash when it doesn't match
                    hash = "sha256-yGkuD2jAG/ACSGkH1Cz8o23wHLtEqtNCUe2kS5OyC68=";
                  };
                };

                buildInputs = oldAttrs.buildInputs ++ extraBuildInputs;
                nativeBuildInputs = oldAttrs.nativeBuildInputs ++ extraNativeBuildInputs;

                # NixOS patch: make the packaged server work on NVIDIA.
                # NVENC dlopen()s libcuda/libnvidia-encode at runtime and NixOS
                # does not put the driver libraries on the default search path.
                # The addDriverRunpath hook does not reach these binaries (their
                # rpath ends up without a driver directory), so patch the real
                # binaries and the monado libraries -- not the small launcher.
                # Without this the server starts and then reports
                # "nvenc not supported" for every codec.
                postFixup = (oldAttrs.postFixup or "") + ''
                  for f in $out/bin/.wivrn-server-wrapped $out/bin/.wivrn-dashboard-wrapped $out/lib/wivrn/*.so*; do
                    if [ -f "$f" ]; then
                      patchelf --add-rpath /run/opengl-driver/lib "$f" || true
                    fi
                  done
                '';

                cmakeFlags = (lib.filter (flag: !lib.hasInfix "GIT_" flag) oldAttrs.cmakeFlags) ++ [
                  (lib.cmakeFeature "GIT_DESC" "nightly")
                  (lib.cmakeFeature "GIT_COMMIT" "nightly")
                  # nixpkgs ships WiVRn VAAPI-only, which cannot encode on an
                  # NVIDIA host at all. Enable NVENC and drop VAAPI so the
                  # packaged server and the AppImage built from it work.
                  (lib.cmakeBool "WIVRN_USE_NVENC" true)
                  (lib.cmakeBool "WIVRN_USE_VAAPI" false)
                ];
              }
            );
          debugPackage = pkgs.enableDebugging package;
        in
        {
          formatter = pkgs.nixfmt;

          packages = {
            default = package;
            debug = debugPackage;
            dissector = package.overrideAttrs (oldAttrs: {
              pname = "wivrn-dissector";
              cmakeFlags = (lib.filter (flag: !(lib.hasPrefix "-DWIVRN_BUILD" flag)) oldAttrs.cmakeFlags) ++ [
                (lib.cmakeBool "WIVRN_BUILD_CLIENT" false)
                (lib.cmakeBool "WIVRN_BUILD_SERVER" false)
                (lib.cmakeBool "WIVRN_BUILD_DASHBOARD" false)
                (lib.cmakeBool "WIVRN_BUILD_WIVRNCTL" false)
                (lib.cmakeBool "WIVRN_BUILD_DISSECTOR" true)
              ];
              buildInputs = oldAttrs.buildInputs ++ [
                (pkgs.symlinkJoin {
                  name = "wireshark-dev-out-combined";
                  paths = [
                    pkgs.wireshark.out
                    pkgs.wireshark.dev
                  ];
                })
              ];
              preFixup = null;
            });
          };
          devShells.default = package.overrideAttrs (oldAttrs: {
            buildInputs = oldAttrs.buildInputs ++ devBuildInputs;
            nativeBuildInputs = oldAttrs.nativeBuildInputs ++ devTools;
          });
        };
    };
}
