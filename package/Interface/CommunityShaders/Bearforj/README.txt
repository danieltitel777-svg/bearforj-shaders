Bearforj menu images. Loaded at menu open from this folder (swap the PNGs, no DLL rebuild).

logo.png                         full helmet wordmark, uncropped, straight RGBA
emotions/thumbs_up.png           compile: progress is moving
emotions/cheering.png            compile: finished, or progress just resumed
emotions/hourglass.png           compile: stalled
emotions/thinking.png            compile: stalled longer; feature off
emotions/sleeping.png            compile: stalled a long time
emotions/surprised.png           warnings
emotions/wink.png                screenshot toast
emotions/bug.png                 shader compile failures
emotions/proud.png waving.png wrench.png sword.png idea.png singing.png pointing.png
brand/                           original logo files, including LOGO_full_helmet.png

Do not crop logo.png. Do not flatten emotions onto white: ImGui blends straight alpha.
tools/bearforj/prepare_mascot_alpha.py rebuilds this folder from a ui_fix drop.
