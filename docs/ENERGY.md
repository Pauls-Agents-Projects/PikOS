# Elering energy-price app

PikOS reads Nord Pool day-ahead data from Elering's public dashboard API; no
account or API key is required. Select Estonia, Finland, Latvia, or Lithuania
in the Energy Prices card.

The source value is EUR/MWh. PikOS converts it to euro-cents/kWh and can add
the configured VAT percentage. For each local calendar day it sorts all
available 15-minute prices into thirds:

- green / low: at or below the lower third;
- yellow / average: the middle third;
- red / high: at or above the upper third.

The app shows the current price and band, the daily average, and an up/down
countdown to the next band crossing. Tomorrow's published points are included
when available, so an evening countdown can extend past midnight.

The graph can cover today, the next 12 hours, the next 24 hours, or today plus
tomorrow. Its connected segments are coloured independently by the same daily
thresholds as the selected icon set: green below the low threshold, yellow in the
middle, and red above the high threshold. The white current-time marker blinks.

Changing bands can show the energy app immediately and play its selected tone.
The low, average, and high icons can each use a separate
[LaMetric gallery](https://developer.lametric.com/icons) ID.

Official references:

- [Elering dashboard](https://dashboard.elering.ee/)
- [Elering dashboard API](https://dashboard.elering.ee/assets/swagger-ui/index.html)
