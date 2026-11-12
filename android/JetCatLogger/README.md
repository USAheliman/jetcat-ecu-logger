# JetCat Logger app (Android)

Shows the logger's own web pages (Fleet, Flights, Trends, Settings, Console) over
**Bluetooth**, so they work at the flying field with no WiFi. The pages are bundled in the
app from `data/index.html`, the same file the logger serves over WiFi.

- **Every flight is copied to the phone and kept.** With the logger switched off, the app
  shows the copies ("on this phone · logger not connected"), and flights deleted from the
  logger stay on the phone.
- **OneDrive (optional):** after each sync, new flights are uploaded to the pilot's OneDrive:
  - `JetCat Logger/JetCat Flights.xlsx`: a Summary sheet, one sheet per heli, and a Notes sheet;
  - `JetCat Logger/Flights/<heli>/<flight>.csv`.

  Uploads wait until the phone has a connection.
- **Read-only:** the app only uses the logger's own routes. It never talks to the ECU.

The Bluetooth link and the page bridge are adapted from Malcolm Messiter's LockDownRadioControl
RXV2App (github.com/Mmessiter/LDRC_V2), used with his permission, GPL-3.0-or-later. So
is this app.

## Getting the app, and updates

**Releases.** Mehul publishes a release with GitHub's **Actions > Release > Run workflow**,
typing the version (e.g. `1.0.0`) and the notes. That builds the app and the logger firmware
for both boards, all stamped with that version, and publishes release `v1.0.0` with
`JetCatLogger.apk`, `firmware-xiao_plus.bin`, `firmware-devkitc.bin` and `release.json`.
Version numbers are only set there; every other build is `dev`.

- **First install:** download `JetCatLogger.apk` from the latest release on the phone and
  open it (allow the browser to install apps when Android asks).
- **App updates:** the app checks when its home screen opens (at most hourly), and from
  Settings > Updates > *Check for app update*. A newer release offers **Update**; the first
  time, Android asks to allow JetCat Logger to install updates.
- **Logger firmware:** Settings > Updates > *Check for firmware updates* lists the releases
  with their notes. **Install** downloads the firmware with the phone's internet and sends it
  to the logger over Bluetooth (about a minute; keep the phone near the logger). The logger
  restarts; tap it in the list to reconnect. PlatformIO uploads over USB or WiFi still work.

**Dev builds.** Every push that touches the app or the pages builds `JetCatLogger.apk` on the
**app-latest** pre-release (`dev-<commit>`), for testing. The update check never offers it.

**Signing key.** Every published build is signed with the same key, so a new build installs
over the old one and keeps the flights on the phone. The key is NOT in the repo: GitHub gets
it from the repository secrets `APP_KEYSTORE_B64` (the .p12 file in base64) and
`APP_KEYSTORE_PASSWORD`. Keep a backup of the .p12 and its password: without them no future
build can update the installed app. (The key used before 12 Nov 2026 was in the repo while it
was private; it was retired when the repo went public.)

## Using it

1. Wake the logger by pressing its button or plugging it into an ECU.
2. Open the app. It connects to the logger used last time, or tap one in the list.
3. The pages open and every new flight is copied to the phone.
4. The back button returns to the home screen, which disconnects so the logger can go back
   to sleep.
5. **View flights on this phone** opens the pages without a logger.

## OneDrive set-up (once)

The app signs in with Microsoft's device-code sign-in (Microsoft identity platform, OAuth
2.0 device authorization grant, personal accounts). That needs a free app registration:

1. Sign in at https://entra.microsoft.com (or portal.azure.com) with your Microsoft
   account and go to **App registrations** → **New registration**.
2. Fill in:
   - Name: `JetCat Logger`
   - Supported account types: **Personal Microsoft accounts only**
   - Redirect URI: leave empty

   Then press **Register**.
3. Copy the **Application (client) ID**.
4. Go to **Authentication** → **Advanced settings**, set **Allow public client flows** to
   **Yes**, and save.
5. Go to **API permissions** → **Add a permission** → Microsoft Graph → Delegated, and add
   `Files.ReadWrite`. `offline_access` is added at sign-in.
6. Put the client ID in GitHub: repo **Settings** → **Secrets and variables** → **Actions**
   → **Variables** → **New variable**, named `ONEDRIVE_CLIENT_ID`. Then re-run the
   **Android app** workflow. Or set it in `gradle.properties`.
7. In the app, go to Settings → **Sign in to OneDrive**. A code is shown, the Microsoft
   page opens, you paste the code and sign in. You only do this once.
