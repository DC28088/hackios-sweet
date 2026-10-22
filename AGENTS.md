# HackiOS Sweet

Read NOTES.md first. Preserve the upstream source pins and licenses. Keep
this repository separate from /home/dan/ios-sweet, whose userdata installation
is still iOS16.7.16 HFS+. HackiOS uses iOS16.5 APFS images; do not overwrite the
existing root with an incompatible image.

Commit completed changes and update NOTES.md at milestones. Compile through
GitHub Actions. Do not add or run tests unless the user asks. No device writes,
flashes or boots are part of a compile request. Confirm destructive device
operations separately. Keep device dumps, serials and proprietary firmware
out of git. A successful compile does not establish a working hardware port
or GPU acceleration; report build, boot and rendering evidence separately.
