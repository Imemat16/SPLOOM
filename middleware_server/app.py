import os
import datetime
import requests
import html
from flask import Flask, request
import alpaca_trade_api as tradeapi
import pandas as pd
from vaderSentiment.vaderSentiment import SentimentIntensityAnalyzer
import re

analyzer = SentimentIntensityAnalyzer()
app = Flask(__name__)

# --- API CONFIGURATION ---
NEWSAPI_KEY = "YOUR_NEWSAPI_KEY_HERE"
FINNHUB_KEY = "YOUR_FINNHUB_KEY_HERE"
ALPACA_KEY = "YOUR_ALPACA_KEY_HERE"
ALPACA_SECRET = "YOUR_ALPACA_SECRET_HERE"
BASE_URL = "https://paper-api.alpaca.markets"

api = tradeapi.REST(ALPACA_KEY, ALPACA_SECRET, BASE_URL, api_version='v2')

def get_tracked_tickers():
    tickers = set([])
    try:
        positions = api.list_positions()
        for p in positions:
            tickers.add(p.symbol)

        orders = api.list_orders(status='all', limit=15)
        for o in orders:
            tickers.add(o.symbol)
    except Exception:
        pass

    if not tickers:
        return ["NO_STOCKS"]

    return sorted(list(tickers))

@app.after_request
def add_header(response):
    response.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate, max-age=0'
    response.headers['Pragma'] = 'no-cache'
    response.headers['Content-Type'] = 'text/plain'
    return response

@app.route('/api/main')
def api_main():
    try:
        account = api.get_account()
        positions = api.list_positions()
        orders = api.list_orders(status='open', limit=5)

        nav = float(account.portfolio_value)
        bp = float(account.buying_power)

        output = f"{nav:.2f}|{bp:.2f}\n"

        if not positions:
            output += "CASH|0.00|0.00|100.0\n"
        else:
            total_invested = sum([float(p.market_value) for p in positions])

            for p in positions:
                sym = p.symbol
                qty = float(p.qty)
                pnl = float(p.unrealized_pl)
                val = float(p.market_value)

                weight = (val / total_invested * 100) if total_invested > 0 else 0.0

                pnl_str = f"+{pnl:.2f}" if pnl > 0 else f"{pnl:.2f}"
                output += f"{sym}|{qty:.2f}|{pnl_str}|{weight:.1f}\n"

        output += "---ORDERS---\n"
        if not orders:
            output += "NONE|0|NONE|NONE\n"
        else:
            for o in orders:
                output += f"{o.symbol}|{o.qty}|{o.side.upper()}|{o.status.upper()}\n"

        return output.strip()
    except Exception as e:
        return f"ERROR|{str(e)}"

@app.route('/api/chart')
def api_chart():
    symbol = request.args.get('symbol', '').strip().upper()
    time_range = request.args.get('range', '1W').upper()

    if not symbol or symbol in ["NO_STOCKS", "API_EMPTY", "NET_FAILED"]:
        flat = ",".join(["50"]*100)
        return f"0.00|0.00\n{flat}\n{flat}\n{flat}\n{flat}"

    try:
        end_date = datetime.datetime.utcnow()
        if time_range == '1D': days = 3
        elif time_range == '1W': days = 14
        elif time_range == '1M': days = 40
        elif time_range == '1Y': days = 365
        elif time_range == 'ALL': days = 1825
        else: days = 14

        start_date = end_date - datetime.timedelta(days=days)
        start_str = start_date.strftime('%Y-%m-%d')

        if time_range == '1D':
            raw_bars = api.get_bars([symbol], tradeapi.TimeFrame.Minute, start=start_str, feed='iex').df
        else:
            raw_bars = api.get_bars([symbol], tradeapi.TimeFrame.Day, start=start_str, feed='iex').df

        if raw_bars.empty: return "ERROR|NO_DATA"

        if isinstance(raw_bars.index, pd.MultiIndex):
            raw_bars = raw_bars.reset_index(level=0, drop=True)
        raw_bars.columns = [str(c).lower() for c in raw_bars.columns]

        close_prices = raw_bars['close'].astype(float)

        sma = close_prices.rolling(window=20, min_periods=1).mean()
        std = close_prices.rolling(window=20, min_periods=1).std().fillna(0)
        upper_bb = sma + (std * 2)
        lower_bb = sma - (std * 2)

        cp_v = close_prices.values
        sma_v = sma.values
        ubb_v = upper_bb.values
        lbb_v = lower_bb.values

        if len(cp_v) > 100:
            indices = [int(i * len(cp_v)/100) for i in range(100)]
            cp_v = [cp_v[i] for i in indices]
            sma_v = [sma_v[i] for i in indices]
            ubb_v = [ubb_v[i] for i in indices]
            lbb_v = [lbb_v[i] for i in indices]

        max_p = max(ubb_v) if len(ubb_v) > 0 else max(cp_v)
        min_p = min(lbb_v) if len(lbb_v) > 0 else min(cp_v)
        spread = max_p - min_p if max_p != min_p else 1

        def normalize(arr):
            return ",".join([str(int(((p - min_p) / spread) * 100)) for p in arr])

        return f"{max_p:.2f}|{min_p:.2f}\n{normalize(cp_v)}\n{normalize(sma_v)}\n{normalize(ubb_v)}\n{normalize(lbb_v)}"

    except Exception as e:
        return f"ERROR|{str(e)[:15]}"

@app.route('/api/portfolio')
def api_portfolio():
    try:
        positions = api.list_positions()
        account = api.get_account()

        raw_labels = []
        sizes = []

        for p in positions:
            val = float(p.market_value)
            if val > 0:
                raw_labels.append(p.symbol)
                sizes.append(val)

        cash = float(account.cash)
        if cash > 0:
            raw_labels.append('CASH')
            sizes.append(cash)

        if not sizes:
            return "EMPTY|100.0|0.0"

        total_val = sum(sizes) if sum(sizes) > 0 else 1.0

        output = ""
        for lbl, val in zip(raw_labels, sizes):
            percentage = (val / total_val) * 100
            output += f"{lbl}|{percentage:.1f}|{val:.2f}\n"

        return output.strip()
    except Exception as e:
        return f"ERROR|{str(e)}"

@app.route('/api/overview')
def api_overview():
    symbol = request.args.get('symbol', '').strip().upper()

    if not symbol or symbol in ["NO_STOCKS", "API_EMPTY", "NET_FAILED"]:
        return "0.00|0.00|0.00|0.00|0.00|0\nN/A|N/A|N/A|N/A|N/A|N/A\nN/A|N/A"

    try:
        end_date = datetime.datetime.utcnow()
        start_date = end_date - datetime.timedelta(days=7)
        start_str = start_date.strftime('%Y-%m-%d')

        raw_bars = api.get_bars([symbol], tradeapi.TimeFrame.Day, start=start_str, feed='iex').df

        if raw_bars.empty: return "ERROR|NO_MARKET_DATA"
        if isinstance(raw_bars.index, pd.MultiIndex): raw_bars = raw_bars.reset_index(level=0, drop=True)
        raw_bars.columns = [str(c).lower() for c in raw_bars.columns]

        close_prices = raw_bars['close'].astype(float).values
        volumes = raw_bars['volume'].astype(float).values
        high_prices = raw_bars['high'].astype(float).values
        low_prices = raw_bars['low'].astype(float).values

        current_price = close_prices[-1]
        prior_price = close_prices[-2] if len(close_prices) > 1 else current_price
        price_change = current_price - prior_price
        pct_change = (price_change / prior_price) * 100 if prior_price != 0 else 0.0

        session_high = max(high_prices[-5:]) if len(high_prices) >= 5 else max(high_prices)
        session_low = min(low_prices[-5:]) if len(low_prices) >= 5 else min(low_prices)
        latest_vol = volumes[-1]

        alpaca_str = f"{current_price:.2f}|{price_change:.2f}|{pct_change:.2f}|{session_high:.2f}|{session_low:.2f}|{int(latest_vol)}"

        try:
            prof_url = f"https://finnhub.io/api/v1/stock/profile2?symbol={symbol}&token={FINNHUB_KEY}"
            prof_data = requests.get(prof_url).json()

            met_url = f"https://finnhub.io/api/v1/stock/metric?symbol={symbol}&metric=all&token={FINNHUB_KEY}"
            met_data = requests.get(met_url).json().get('metric', {})

            mcap_raw = prof_data.get('marketCapitalization', 0)
            if mcap_raw and mcap_raw > 1000: mcap = f"{mcap_raw/1000:.2f}B"
            elif mcap_raw: mcap = f"{mcap_raw:.2f}M"
            else: mcap = "N/A"

            pe = met_data.get('peExclExtraTTM', 'N/A')
            if isinstance(pe, (float, int)): pe = f"{pe:.2f}"

            pb = met_data.get('pbAnnual', 'N/A')
            if isinstance(pb, (float, int)): pb = f"{pb:.2f}"

            eps = met_data.get('epsTTM', 'N/A')
            if isinstance(eps, (float, int)): eps = f"{eps:.2f}"

            div = met_data.get('dividendYieldIndicatedAnnual', 0)
            div_str = f"{div:.2f}%" if div else "N/A"

            beta = met_data.get('beta', 'N/A')
            if isinstance(beta, (float, int)): beta = f"{beta:.2f}"

            sector = str(prof_data.get('finnhubIndustry', 'N/A'))[:25]
            industry = "EQUITY" 

            fund_str = f"{pe}|{pb}|{eps}|{div_str}|{mcap}|{beta}"
            prof_str = f"{sector}|{industry}"

        except Exception:
            fund_str = "N/A|N/A|N/A|N/A|N/A|N/A"
            prof_str = "N/A|N/A"

        return f"{alpaca_str}\n{fund_str}\n{prof_str}"

    except Exception as e:
        err_msg = str(e).replace('\n', ' ')
        if len(err_msg) > 30: err_msg = err_msg[:27] + "..."
        return f"ERROR|{err_msg}"

@app.route('/api/news')
def api_news():
    symbol = request.args.get('symbol', '').strip().upper()

    if not symbol or symbol in ["NO_STOCKS", "API_EMPTY", "NET_FAILED"]:
        return "SYSTEM|[=]|EXECUTE TRADES TO POPULATE WIRE| "

    try:
        news_url = f"https://newsapi.org/v2/everything?q={symbol}&language=en&sortBy=publishedAt&pageSize=7&apiKey={NEWSAPI_KEY}"
        res = requests.get(news_url)
        data = res.json()

        if data.get('status') == 'error':
            return f"ERROR|[=]|{data.get('message')}| "

        articles = data.get('articles', [])
        if not articles:
            return f"WIRE|[=]|NO RECENT HEADLINES FOR {symbol}| "

        output = ""
        for item in articles:

            headline = item.get('title', 'No Title')
            headline = html.unescape(headline).replace('‘', "'").replace('’', "'").replace('“', '"').replace('”', '"').replace('–', '-').replace('—', '-')
            headline = headline.encode('ascii', 'ignore').decode('ascii').replace('\n', ' ').replace('|', ' ')

            raw_desc = item.get('description', '') or item.get('content', '')
            if not raw_desc:
                clean_summary = "No summary provided by the wire."
            else:

                clean_summary = re.sub(r'\s*\[\+\d+\s*chars\]\s*', '', raw_desc).strip()

                last_punct = max(clean_summary.rfind('.'), clean_summary.rfind('!'), clean_summary.rfind('?'))

                if last_punct > 30:
                    clean_summary = clean_summary[:last_punct+1]
                else:
                    if len(clean_summary) > 180:
                        clean_summary = clean_summary[:175].rsplit(' ', 1)[0] + "..."
                    else:
                        clean_summary = clean_summary + "..."

            clean_summary = html.unescape(clean_summary).replace('‘', "'").replace('’', "'").replace('“', '"').replace('”', '"').replace('–', '-').replace('—', '-')
            clean_summary = clean_summary.encode('ascii', 'ignore').decode('ascii').replace('\n', ' ').replace('|', ' ')

            clean_summary = clean_summary[:490]

            source = item.get('source', {}).get('name', 'WIRE').upper()
            source = source.encode('ascii', 'ignore').decode('ascii')[:10]

            score = analyzer.polarity_scores(headline)['compound']
            if score >= 0.05: sentiment = "[+]"
            elif score <= -0.05: sentiment = "[-]"
            else: sentiment = "[=]"

            if len(headline) > 50: headline = headline[:47] + "..."

            output += f"{source}|{sentiment}|{headline}|{clean_summary}\n"

        return output.strip()
    except Exception as e:
        err_msg = str(e).replace('\n', ' ')
        if len(err_msg) > 30: err_msg = err_msg[:27] + "..."
        return f"ERROR|[=]|{err_msg}| "

@app.route('/api/tickers')
def api_tickers():
    try:
        tickers = get_tracked_tickers()
        return ",".join(tickers)
    except Exception:
        return "NO_STOCKS"


@app.route('/api/order', methods=['GET'])
def place_order():

    symbol = request.args.get('sym', '').upper().strip()
    side = request.args.get('side', '').lower()      
    qty_type = request.args.get('qtype', '').upper()   
    qty_val = request.args.get('qty', '0')
    order_type = request.args.get('type', '').lower() 
    tif = request.args.get('tif', '').lower()         

    if not symbol:
        return "ERROR|MISSING TICKER SYMBOL"
    try:
        qty_float = float(qty_val)
        if qty_float <= 0:
            return "ERROR|QTY MUST BE GREATER THAN 0"
    except ValueError:
        return "ERROR|INVALID QTY VALUE"

    try:

        formatted_type = order_type
        if formatted_type == "stop-limit":
            formatted_type = "stop_limit"
        elif formatted_type == "trailing-stop":
            formatted_type = "trailing_stop"

        if qty_type == "DOLLARS":
            order = api.submit_order(
                symbol=symbol,
                notional=qty_float,
                side=side,
                type=formatted_type,
                time_in_force=tif
            )
        else:
            order = api.submit_order(
                symbol=symbol,
                qty=qty_float,
                side=side,
                type=formatted_type,
                time_in_force=tif
            )

        return f"SUCCESS|{side.upper()} ORDER FOR {symbol} PLACED! ID: {order.id[:8]}"

    except Exception as e:
        return f"ERROR|{str(e)}"

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)