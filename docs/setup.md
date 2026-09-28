# Setting up the Librarian

All settings are entered on a web page served by the device itself. They are stored in the device's flash (NVS) and survive reboots and reflashing of the app.

## First-time setup (no Wi-Fi saved yet)

1. Power the device. After the start-up light sequence it stays on **spinning yellow** (still waiting, because it has no Wi-Fi to join): it is running its own Wi-Fi network.
2. On a phone or PC, join the Wi-Fi network **Librarian-XXXX** (XXXX = the last four characters of the device's MAC address). Password: the value of `CONFIG_BOOKBOOK_SETUP_AP_PASSWORD` (default `bookbook1`; override it in your git-ignored `secrets/sdkconfig.secrets`).
3. A setup page should open automatically. If not, browse to **http://192.168.4.1/**.
4. Press **Scan for networks**, pick your Wi-Fi, enter its password, enter the library login, and press **Save**, then **Restart device**.
5. After the restart the device joins your Wi-Fi, says its greeting, and the light goes **green** when it is ready.

No page password is asked for on the setup network the first time, because you have already had to know the Wi-Fi password to be there.

## Changing settings later

Browse to **http://librarian.local/** (or the IP address the device logs at start-up). The browser asks for a password: user name **admin**, and the factory default password (the value of `CONFIG_BOOKBOOK_ADMIN_PASSWORD`, `bookbook` unless you override it in your git-ignored `secrets/sdkconfig.secrets`). The password is baked into the firmware and cannot be changed on the page.

Note: this page is plain HTTP on your home network, so anyone on that network who knows the password can read and change the settings. Saved passwords are never sent back to the page (it only shows whether each is set).

## What the LEDs mean

Three looks: **steady**, **spinning**, or the whole ring **flashing**. A spinner is one bright LED circling the ring
once every two seconds with a fading 3-LED tail, and it always means the device is **waiting**.

| LEDs | Meaning |
|---|---|
| red, green, blue, then spinning yellow | starting up (and still spinning yellow while it is not online). The intro is spoken in low red, then it turns green |
| low green | ready for touch-to-talk |
| low blue | the pad is touched: it is listening |
| spinning blue | you let go: waiting for the answer |
| low red | something is being spoken: the intro at start-up, or an answer |
| low green again | finished, ready for the next touch |
| low green with one low-yellow LED | ready, and an update is available (nothing is said about it) |
| whole ring flashing yellow (2 Hz) | a firmware update is downloading |

Red comes on when the sound actually starts, not when she begins preparing the answer, so spinning blue lasts until
she is about to speak.

## The button

- **Volume by voice:** ask her to turn it up or down, or to set it to a number from 0 to 100 (the same scale as the setup page's slider). She changes it at once, says the new level, and remembers it.
- **Touch and hold the pad** (the touch pad on D5): push-to-talk. Speak while holding (up to 30 s; if you go on longer she says "I'm sorry, but I can only listen up to 30 seconds at a time" and answers what she heard), release, and it answers. Press again while it is answering to stop it.
- A touch shorter than 0.7 s is ignored.

**Setup mode by voice.** There is no button gesture for it (it was removed to free the button for talking). Ask about setup mode, the setup page, or changing the Wi-Fi, and she asks whether you would like her to create a wireless access point for setup. Say yes, and she opens the Librarian-XXXX network, reads its name out and the address to browse to (**librarian.local**), and keeps it open until **10 minutes after the setup page was last used** (or 10 minutes after she opened it, if nobody connects). Join it with the setup-network password and use the page as usual; the admin password is required. She never says a password aloud. To close it sooner, tell her to turn off setup mode. Otherwise use the setup page over your normal Wi-Fi. The setup network also starts by itself when no Wi-Fi is saved or the saved one cannot be joined; that one stays up until the Wi-Fi works.

Do **not** hold BOOT while powering up or resetting the board: it is a strapping pin, and holding it at reset puts the chip into firmware-download mode instead of running the app.

## If Wi-Fi fails

If the saved network cannot be joined within 20 seconds, the device also starts the Librarian-XXXX network (still retrying your Wi-Fi in the background). In this case the admin password **is** required.

## Known limitations

- The setup-network password and the admin password are the same on every device (both are build settings baked into the firmware). Fine for bench use; make them per-device before giving a unit to anyone.
- Settings travel over unencrypted HTTP (setup network is WPA2-encrypted; your LAN is only as private as your Wi-Fi).

## What is editable and what is baked in

The Librarian is a single-user device, so the setup page only edits the member's **environment**: the Wi-Fi network name and password, the library login, and the speaker volume. These are saved on the device; the values in `secrets/sdkconfig.secrets` are only the defaults for a freshly flashed board, and anything saved on the page wins.

Everything the program itself needs is **baked into the firmware at build time** from the git-ignored `secrets/sdkconfig.secrets`, and cannot be changed on the device: `CONFIG_BOOKBOOK_AZURE_SPEECH_KEY` / `_REGION`, `CONFIG_BOOKBOOK_ANTHROPIC_KEY`, `CONFIG_BOOKBOOK_MEMORY_URL` (a container-scoped SAS URL: the storage account key never goes on the device), `CONFIG_BOOKBOOK_ADMIN_PASSWORD`, `CONFIG_BOOKBOOK_SETUP_AP_PASSWORD` and `CONFIG_BOOKBOOK_PRACTICE_MODE` (pretend to add/remove books). Change one by editing that file, clearing the generated `sdkconfig`, and reflashing. Older builds saved some of these on the device; they are erased from its saved settings at start-up.

Nothing on the device is encrypted, so anyone who copies its flash memory can read the baked-in keys.

## Updating the firmware over the air

An update is only ever installed when the member asks for it out loud ("is there an update?", "update yourself"). Nothing checks or installs by itself.

**Publishing (developer):** build, then run `python tools/publish_firmware.py`. It uploads `build/bookbook.bin` and then a small manifest, `bookbook.json`, to the same storage container as `memory.json`, using the SAS URL in `secrets/sdkconfig.secrets` (which needs create and write permission). `--dry-run` shows what would be published and `--status` shows what is published now. Uploading a different build is what makes an update "available": the device compares the image's ELF hash, so no version number needs bumping. `tools\release.ps1` does the same as part of a numbered release (tag, build, push, GitHub release); the version a unit reports comes from `git describe` at build time, so a build of an untagged or uncommitted tree reports something like `v2.0.7-1-gabc1234-dirty`.

**On the device:** shortly after start-up it quietly checks for an update, and if there is one the green ready light gets a single low-yellow LED (nothing is said). "Is there an update?" reads only the manifest. "Update yourself" queues the install; it starts once her reply has been spoken, with the whole ring **flashing yellow (2 Hz)** while it downloads, and the device restarts. After the restart it says "The update is complete" before its greeting. If the download or the image is bad, it says so straight away and carries on as before; if the new image crashes and the bootloader goes back, the next start says "The update did not work, so I have gone back to my previous version". A new image must run for 30 seconds to be kept; if it crashes or resets before then, the bootloader goes back to the previous image (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

The first build with OTA support must be flashed over USB (the bootloader changes), and so must any build that changes the partition table (`partitions.csv`). Otherwise updates go over the air.

The image contains the baked-in keys, so the container must stay private. It is reachable only with the SAS token, which is already inside the firmware.
