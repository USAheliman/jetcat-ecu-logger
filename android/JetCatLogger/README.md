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

## Getting the app

GitHub builds it on every change (`.github/workflows/android-app.yml`). The installable file
is `JetCatLogger.apk` on the repo's **app-latest** release.

1. On the phone, open the release page in GitHub (signed in, because the repo is private).
2. Download `JetCatLogger.apk` and open it.
3. The first time, allow your browser to install apps when Android asks.

Every build is signed with the same key (`keystore/jetcat-logger.p12`), so a new build
installs over the old one and keeps the flights stored on the phone.

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
