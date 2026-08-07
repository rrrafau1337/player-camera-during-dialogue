includes("lib/commonlibsf")

set_project("PlayerCameraDuringDialogue")
set_version("1.18.12")
set_license("GPL-3.0-or-later")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("PlayerCameraDuringDialogue")
    set_basename("PointCameraAtPlayer")

    add_rules("commonlibsf.plugin", {
        name = "PointCameraAtPlayer",
        author = "rrrafau",
        description = "A native SFSE plugin that adds cinematic player-focused dialogue cameras to Starfield.",
        email = ""
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")