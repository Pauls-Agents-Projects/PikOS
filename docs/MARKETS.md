# Stocks and foreign exchange

PikOS offers two providers:

- **Yahoo Finance** is the default and needs no account or API key. PikOS
  uses its public chart endpoint for shares and index futures.
- **Frankfurter** supplies keyless daily reference-rate history for slash
  currency pairs such as `USD/GBP`.
- **Kraken** supplies keyless OHLC history for `BTC-EUR`, `BTC-USD`, and
  `BTC-GBP`.
- **Twelve Data** remains available when a keyed, documented API is preferred.

## Setup

1. Open PikOS settings and select **Yahoo Finance — no account**.
2. Add up to six ticker/icon rows, for example:

   ```text
   AAPL
   AIR.PA
   USD/GBP
   BTC-EUR
   NQ=F
   ```

3. Enable the **Stocks & forex** local app, choose its animation, then save.
4. Use **Refresh online data** to fetch immediately.

Positive changes are green, negative changes are red, and unchanged quotes are
amber. PikOS calculates the percentage against the provider's previous
close.

The chart period can be set to one day, five days, one month, six months, or
one year. PikOS chooses a matching Yahoo interval and downsamples it to the
23-pixel graph. The latest point blinks only while the provider reports that
the market is open.

For Yahoo, `EUR/USD` is translated to `EURUSD=X` automatically. The Yahoo
symbol for Airbus SE in Paris is `AIR.PA`; currency characters such as `$`,
`€`, and `£` belong to the rendered price and are not part of a ticker.
Keyless forex rows use Frankfurter daily reference rates and supported Bitcoin
pairs use Kraken OHLC data. The Yahoo endpoint is widely used but is not a
supported public API and can change or rate-limit without notice. Choose
Twelve Data and enter a key if that tradeoff is unsuitable. The default
20-minute interval is deliberately conservative. Values that cannot fit the
23-pixel text area are compacted with `K`, `M`, `B`, or `T` before the currency
symbol.

Official references:

- [Twelve Data API documentation](https://twelvedata.com/docs)
- [Quote endpoint](https://twelvedata.com/docs#quote)
- [Twelve Data pricing and quotas](https://twelvedata.com/pricing)
- [Official ECB exchange-rate data API](https://data-api.ecb.europa.eu/)
- [Frankfurter API](https://frankfurter.dev/)
- [Kraken OHLC endpoint](https://docs.kraken.com/api/docs/rest-api/get-ohlc-data/)
