# RommPS

The native PS5 app for RomM Sync (`PPSA76677`): your RomM library, downloads, save sync status and settings, on the TV with the controller. It talks to the RomM Sync payload on the console (its local API on port 8780), so the payload has to be running; RommPS shows how to load it when it isn't.

It's built on mihawk-99's [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate) (at `b577e950`): Sascha Willems' Vulkan example base class with its PS5 hooks, RADV for the GPU, and BlackBearReloaded's UI kit drawn with Vulkan through mihawk-99's PS5_VKHomebrewUI. The app itself is `examples/rommps/rommps.cpp` and the screens in `examples/rommps/kit/`.

## Building and running

It builds against mihawk-99's PS5 stack, checked out in `../ps5-stack` beside this repository (PS5_Vulkan, the payload SDK fork, PS5_Mesa), inside the Docker image from `platform/ps5/app/docker`. From the repository's root:

```bash
tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/rommps && bash ps5/tools/build.sh rommps'   # dist/PPSA76677/
tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/rommps && bash ps5/tools/run.sh rommps'     # deploy and a test run
tools/release-app.sh v1.2.3                                                                           # the release zip
```

The menu sounds aren't in git. Put them in `platform/ps5/app/assets/sounds/` (`focus_01.wav`, `back_01.wav` and so on, named after the kit's cues) before building; without them the kit's own sounds are used.

## Licences

The UI kit is GPL-3.0-or-later, and so is RommPS as distributed: its screens, the UI module (`ps5/ui/`) and the kit's sounds (`ps5/licenses/GPL-3.0.txt`). The kit's fonts are OFL-1.1 and Bitstream Vera. The rest of the template is MIT (`LICENSE.md`: Sascha Willems', and mihawk-99's PS5 layer). The menu sounds are Konami's, used with permission (`ps5/SOUNDS-LICENCE.txt`). Every asset and its licence is listed in `ps5/ASSETS.md`, and the built app carries all of it in `LEGAL.txt` and `licenses/`.
