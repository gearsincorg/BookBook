# Setting up a BookBook

All settings are entered on a web page served by the device itself. They are stored in the device's flash (NVS) and survive reboots and reflashing of the app.

## First-time setup (no Wi-Fi saved yet)

1. Power the device. After the start-up light sequence it shows **purple**: it is running its own Wi-Fi network.
2. On a phone or PC, join the Wi-Fi network **BookBook-XXXX** (XXXX = the last four characters of the device's MAC address). Password: the value of `CONFIG_BOOKBOOK_SETUP_AP_PASSWORD` (default `bookbook1`; override it in your git-ignored `secrets/sdkconfig.secrets`).
3. A setup page should open automatically. If not, browse to **http://192.168.4.1/**.
4. Press **Scan for networks**, pick your Wi-Fi, enter its password, fill in the library, Azure and Claude details, and press **Save**, then **Restart device**.
5. The light goes **blue** when the device is online.

No page password is asked for on the setup network the first time, because you have already had to know the Wi-Fi password to be there.

## Changing settings later

Browse to **http://bookbook.local/** (or the IP address the device logs at start-up). The browser asks for a password: user name **admin**, password **bookbook** until you change it in the "This page" section. Change it.

Note: this page is plain HTTP on your home network, so anyone on that network who knows the password can read and change the settings. Saved secrets are never sent back to the page (it only shows whether each is set).

## The button

- **Hold**: push-to-talk. Speak while holding (up to 15 s), release, and it answers. Press again while it is answering to stop it.
- A very quick tap (under 0.4 s) is ignored.

There is currently no button gesture for setup mode (it was removed to free the button for talking). Use the setup page over your normal Wi-Fi. The BookBook-XXXX setup network still starts by itself when no Wi-Fi is saved or the saved one cannot be joined.

Do **not** hold BOOT while powering up or resetting the XIAO stand-in: it is a strapping pin, and holding it at reset puts the chip into firmware-download mode instead of running the app.

## If Wi-Fi fails

If the saved network cannot be joined within 20 seconds, the device also starts the BookBook-XXXX network (still retrying your Wi-Fi in the background). In this case the admin password **is** required.

## Known limitations

- The setup-network password and the initial admin password are the same on every device (the setup-network one is a build setting, the admin one a fixed default). Fine for bench use; make them per-device before giving a unit to anyone.
- Settings travel over unencrypted HTTP (setup network is WPA2-encrypted; your LAN is only as private as your Wi-Fi).
- Developer defaults in `secrets/sdkconfig.secrets` are used until something is saved through the page, and saved values then win.
