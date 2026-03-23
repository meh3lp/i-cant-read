# Build and install

1) Prepare build dir
    `cmake --preset plugin-dev`
2) Build
    `cmake --build build_x86_64`
3) Find the files
    `ls ./build_x86_64/rundir/RelWithDebInfo`
4) Copy the plugin files to `~/.config/obs-studio/plugins` (for linux)
    `./copy_build_to_config.sh`
