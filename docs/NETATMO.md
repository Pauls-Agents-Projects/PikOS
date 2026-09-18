# Netatmo Weather Station

PikOS calls Netatmo's `getstationsdata` API directly. No Home Assistant,
Node-RED, or PikOS cloud is required.

Official references:

- [Netatmo authentication](https://dev.netatmo.com/apidocumentation/oauth)
- [Weather API](https://dev.netatmo.com/apidocumentation/weather)
- [`getstationsdata`](https://dev.netatmo.com/dev/resources/technical/reference/weatherstation/getstationsdata)

## Obtain tokens

1. Create an application in the Netatmo developer portal.
2. Add this redirect URI exactly:

   ```text
   http://localhost:8765/callback
   ```

3. From this repository run:

   ```sh
   python3 tools/pikos_credentials.py netatmo \
     --client-id YOUR_CLIENT_ID
   ```

   Enter the application client secret at the hidden prompt. Secrets are not
   accepted as command-line arguments, so they do not enter shell history or a
   process listing.

4. Approve the `read_station` scope in the opened browser.
5. Paste the three JSON values into PikOS → Climate → Netatmo. Leave **Access
   token** blank: PikOS obtains a short-lived access token from the printed
   refresh token during the next central sync.
6. Enable the Netatmo local app and save.

The helper only listens on `127.0.0.1`, validates an OAuth state nonce, and
does not save a token file. Its stdout is copyable JSON. It never prints the
one-time authorization code or short-lived access token; the JSON still
contains the persistent client secret and refresh token, so paste it on a
trusted computer and clear the terminal afterwards.

## Selecting the station

Normally PikOS selects the first `NAModule1` (outdoor module). To make the
selection explicit, enter its `_id` in **Outdoor module ID**. Module ID wins
over module name. Station ID is optional when the Netatmo account has only one
weather station. Rain (`NAModule3`) and wind (`NAModule2`) modules are found
automatically in the same station response; they do not need separate IDs.

Access tokens expire. PikOS uses the saved refresh token, client ID, and
client secret to obtain a new access/refresh token pair after a `401`, then
stores the rotated refresh token in NVS.

## Data shown and rain alerts

The Climate app rotates through outdoor temperature, humidity, wind speed and
rain when those modules are present. Gust speed, wind direction, current rain
rate, and one-hour/24-hour rain totals are retained in the compact local cache
and exposed to the settings page.

Temperature uses a generated five-level thermometer instead of a fixed gallery
image. Its mercury column and colour move from blue to red across bands chosen
for Estonian conditions: below -5 °C, -5–2 °C, 2–10 °C, 10–18 °C, and 18 °C or
warmer. The bands are deliberately broad ambient categories informed by the
[Estonian Environment Agency's 1991–2020 temperature normals](https://www.ilmateenistus.ee/kliima/kliimanormid/ohutemperatuur/?lang=en),
not meteorological warning thresholds.

PikOS can show and chime on a dry-to-raining transition. The first wet reading
after a reboot establishes a quiet baseline so restarting during rain does not
create a false alert. Netatmo cloud station readings normally update on a
multi-minute cadence, so this is an ambient warning rather than a real-time
rain detector.
