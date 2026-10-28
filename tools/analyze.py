#!/usr/bin/env python3
"""
Paxcounter-Auswertung
=====================

Liest die CSV-Dateien von der SD-Karte des Paxcounters und erzeugt einen
interaktiven HTML-Bericht (eine Datei, offline lesbar) sowie eine Tagestabelle
als CSV.

    python analyze.py E:\\                     # SD-Karte direkt
    python analyze.py C:\\Daten\\Event2026     # oder ein kopierter Ordner
    python analyze.py E:\\ --von 2026-11-06 --bis 2026-11-08 -o bericht.html

Voraussetzungen:  pip install pandas plotly

Verarbeitet werden alle raw_YYYY-MM-DD.csv im Ordner. Grundlage sind immer die
Rohdaten; die 10-Minuten-Peaks und Tageswerte werden daraus neu berechnet, damit
alte und neue Firmware-Stände einheitlich ausgewertet werden:

  * Firmware < 1.2: Zeitstempel in UTC mit "Z"  -> wird nach MEZ/MESZ umgerechnet
  * Firmware >= 1.2: Zeitstempel bereits in Lokalzeit (MEZ/MESZ)
  * abgeschnittene letzte Zeilen, doppelte Kopfzeilen, Duplikate werden verworfen
"""

from __future__ import annotations

import argparse
import html
import sys
from datetime import date
from pathlib import Path

try:
    import pandas as pd
    import plotly.graph_objects as go
    from plotly.subplots import make_subplots
except ImportError:  # pragma: no cover
    sys.exit("Bitte zuerst installieren:  pip install pandas plotly")

TZ = "Europe/Berlin"
INTERVAL = "10min"          # wie STATS_INTERVAL_SECONDS in der Firmware
PEAK_THRESHOLD = 0.80       # wie PEAK_PHASE_THRESHOLD in der Firmware
WEEKDAYS = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]

# Farben (validierte Referenzpalette, helle Darstellung)
C = {
    "surface": "#fcfcfb",
    "page": "#f9f9f7",
    "ink": "#0b0b0b",
    "ink2": "#52514e",
    "muted": "#898781",
    "grid": "#e1e0d9",
    "axis": "#c3c2b7",
    "ble": "#2a78d6",       # Serie 1
    "wifi": "#eb6834",      # Serie 2
    "band": "rgba(42,120,214,0.16)",
}
CATEGORICAL = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100",
               "#e87ba4", "#008300", "#6250d6", "#e34948"]
SEQUENTIAL = ["#f0efec", "#cde2fb", "#9ec5f4", "#6da7ec",
              "#3987e5", "#256abf", "#184f95", "#0d366b"]
FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'


# ---------------------------------------------------------------------------
# Einlesen
# ---------------------------------------------------------------------------
def parse_timestamps(s: pd.Series) -> pd.Series:
    """ISO-8601 -> naive Lokalzeit. 'Z'-Zeitstempel (UTC) werden umgerechnet."""
    s = s.astype(str).str.strip()
    is_utc = s.str.endswith("Z")
    out = pd.Series(pd.NaT, index=s.index, dtype="datetime64[ns]")
    if is_utc.any():
        t = pd.to_datetime(s[is_utc], format="%Y-%m-%dT%H:%M:%SZ",
                           errors="coerce", utc=True)
        out[is_utc] = t.dt.tz_convert(TZ).dt.tz_localize(None)
    if (~is_utc).any():
        out[~is_utc] = pd.to_datetime(s[~is_utc], format="%Y-%m-%dT%H:%M:%S",
                                      errors="coerce")
    return out


def load_raw(folder: Path) -> tuple[pd.DataFrame, list[str]]:
    files = sorted(folder.glob("raw_*.csv"))
    notes: list[str] = []
    frames = []
    for f in files:
        try:
            df = pd.read_csv(f, dtype=str, on_bad_lines="skip",
                             encoding_errors="replace")
        except pd.errors.EmptyDataError:
            continue
        if "Timestamp" not in df.columns:
            notes.append(f"{f.name}: keine Spalte 'Timestamp', übersprungen")
            continue
        n0 = len(df)
        df["ts"] = parse_timestamps(df["Timestamp"])
        if df["Timestamp"].astype(str).str.endswith("Z").any():
            notes.append(f"{f.name}: alte Firmware (UTC) – nach MEZ/MESZ umgerechnet")
        for col in ("WiFi", "BLE", "Voltage_mV"):
            if col in df.columns:
                df[col] = pd.to_numeric(df[col], errors="coerce")
        df = df.dropna(subset=["ts", "BLE"])
        dropped = n0 - len(df)
        if dropped:
            notes.append(f"{f.name}: {dropped} ungültige/abgeschnittene Zeile(n) verworfen")
        frames.append(df.drop(columns=["Timestamp"]))
    if not frames:
        return pd.DataFrame(columns=["ts", "WiFi", "BLE"]), notes
    raw = (pd.concat(frames, ignore_index=True)
           .drop_duplicates(subset=["ts"])
           .sort_values("ts")
           .reset_index(drop=True))
    if "WiFi" not in raw.columns:
        raw["WiFi"] = 0
    raw["date"] = raw["ts"].dt.date
    return raw, notes


# ---------------------------------------------------------------------------
# Kennzahlen
# ---------------------------------------------------------------------------
def interval_peaks(raw: pd.DataFrame) -> pd.DataFrame:
    """10-Minuten-Peaks (nur Intervalle mit Messwerten)."""
    g = (raw.set_index("ts")[["BLE", "WiFi"]]
         .resample(INTERVAL).max()
         .dropna(how="all")
         .reset_index())
    g["date"] = g["ts"].dt.date
    return g


def daily_table(raw: pd.DataFrame, peaks: pd.DataFrame) -> pd.DataFrame:
    rows = []
    for d, p in peaks.groupby("date"):
        r = raw[raw["date"] == d]
        ble_max = int(p["BLE"].max())
        i = p["BLE"].idxmax()
        peak_minutes = int((p["BLE"] >= PEAK_THRESHOLD * ble_max).sum()) * 10 if ble_max else 0
        wifi_max = int(p["WiFi"].max()) if p["WiFi"].notna().any() else 0
        rows.append({
            "Datum": d,
            "Tag": WEEKDAYS[d.weekday()],
            "Messung_von": r["ts"].min().strftime("%H:%M"),
            "Messung_bis": r["ts"].max().strftime("%H:%M"),
            "Messwerte": len(r),
            "BLE_Peak": ble_max,
            "BLE_Peak_Zeit": p.loc[i, "ts"].strftime("%H:%M"),
            "BLE_Mittel": round(float(r["BLE"].mean()), 1),
            "BLE_Peakdauer_min": peak_minutes,
            "WiFi_Peak": wifi_max,
            "WiFi_Mittel": round(float(r["WiFi"].mean()), 1),
        })
    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# Diagramme
# ---------------------------------------------------------------------------
def tod(ts: pd.Series) -> pd.Series:
    """Datum auf einen Referenztag setzen -> alle Tage auf einer Uhrzeit-Achse."""
    return pd.to_datetime("2000-01-01 " + ts.dt.strftime("%H:%M:%S"))


def base_layout(fig: go.Figure, height: int, title: str | None = None) -> None:
    fig.update_layout(
        height=height,
        margin=dict(l=56, r=24, t=48 if title else 16, b=40),
        title=dict(text=title, x=0, font=dict(size=15, color=C["ink"])) if title else None,
        paper_bgcolor=C["surface"], plot_bgcolor=C["surface"],
        font=dict(family=FONT, size=12, color=C["ink2"]),
        hovermode="x unified",
        hoverlabel=dict(bgcolor="white", font=dict(family=FONT, color=C["ink"])),
        legend=dict(orientation="h", yanchor="bottom", y=1.02, x=0,
                    font=dict(color=C["ink2"])),
    )
    fig.update_xaxes(showgrid=False, linecolor=C["axis"], tickcolor=C["axis"],
                     tickfont=dict(color=C["muted"]))
    fig.update_yaxes(gridcolor=C["grid"], zeroline=False, linecolor=C["axis"],
                     tickfont=dict(color=C["muted"]), rangemode="tozero")


def chart_days(raw: pd.DataFrame, peaks: pd.DataFrame, days: list[date]) -> go.Figure:
    n = len(days)
    titles = [f"{WEEKDAYS[d.weekday()]} {d:%d.%m.%Y}" for d in days]
    fig = make_subplots(rows=n, cols=1, shared_xaxes=True, vertical_spacing=0.35 / max(n, 1),
                        subplot_titles=titles)
    ymax = float(peaks["BLE"].max() or 1) * 1.1
    for k, d in enumerate(days, start=1):
        r = raw[raw["date"] == d]
        p = peaks[peaks["date"] == d]
        first = k == 1
        # Peak-Phase (>= 80% Tagesmax) als Hintergrundband
        dmax = p["BLE"].max()
        if dmax and dmax > 0:
            hot = tod(p.loc[p["BLE"] >= PEAK_THRESHOLD * dmax, "ts"]).sort_values().tolist()
            step = pd.Timedelta(INTERVAL)
            start = prev = None
            for t in hot + [None]:  # zusammenhängende Intervalle zu Bändern verbinden
                if start is not None and (t is None or t - prev > step):
                    fig.add_vrect(x0=start, x1=prev + step, fillcolor=C["band"],
                                  line_width=0, layer="below", row=k, col=1,
                                  exclude_empty_subplots=False)
                    start = None
                if t is not None:
                    start = t if start is None else start
                    prev = t
        fig.add_trace(go.Scatter(
            x=tod(r["ts"]), y=r["BLE"], mode="lines", name="BLE Rohwert",
            line=dict(color="rgba(42,120,214,0.30)", width=1),
            legendgroup="raw", showlegend=first,
            hovertemplate="BLE roh %{y}<extra></extra>"), row=k, col=1)
        fig.add_trace(go.Scatter(
            x=tod(p["ts"]), y=p["BLE"], mode="lines", name="BLE 10-Min-Peak",
            line=dict(color=C["ble"], width=2, shape="hv"),
            legendgroup="ble", showlegend=first,
            hovertemplate="BLE Peak %{y}<extra></extra>"), row=k, col=1)
        fig.add_trace(go.Scatter(
            x=tod(p["ts"]), y=p["WiFi"], mode="lines", name="WiFi 10-Min-Peak",
            line=dict(color=C["wifi"], width=2, shape="hv"),
            legendgroup="wifi", showlegend=first,
            hovertemplate="WiFi Peak %{y}<extra></extra>"), row=k, col=1)
        fig.update_yaxes(range=[0, ymax], title_text="Geräte" if k == (n + 1) // 2 else None,
                         row=k, col=1)
    fig.update_xaxes(tickformat="%H:%M", dtick=2 * 3600 * 1000)
    base_layout(fig, height=max(260, 210 * n))
    fig.update_annotations(font=dict(size=13, color=C["ink"]), x=0, xanchor="left")
    return fig


def chart_compare(peaks: pd.DataFrame, days: list[date]) -> go.Figure:
    fig = go.Figure()
    shown = days[-len(CATEGORICAL):]  # max. 8 Farben, ältere Tage in Grau
    for d in days:
        p = peaks[peaks["date"] == d]
        if d in shown:
            color, width = CATEGORICAL[shown.index(d)], 2
        else:
            color, width = C["axis"], 1
        fig.add_trace(go.Scatter(
            x=tod(p["ts"]), y=p["BLE"], mode="lines",
            name=f"{WEEKDAYS[d.weekday()]} {d:%d.%m.}",
            line=dict(color=color, width=width),
            hovertemplate=f"{d:%d.%m.}: %{{y}}<extra></extra>"))
    base_layout(fig, 380)
    fig.update_xaxes(tickformat="%H:%M", dtick=2 * 3600 * 1000)
    fig.update_yaxes(title_text="BLE-Geräte (10-Min-Peak)")
    return fig


def chart_heatmap(raw: pd.DataFrame, days: list[date]) -> go.Figure:
    h = raw.assign(hour=raw["ts"].dt.hour).groupby(["date", "hour"])["BLE"].max().unstack()
    h = h.reindex(index=days, columns=range(24))
    labels = [f"{WEEKDAYS[d.weekday()]} {d:%d.%m.}" for d in h.index]
    scale = [[i / (len(SEQUENTIAL) - 1), c] for i, c in enumerate(SEQUENTIAL)]
    fig = go.Figure(go.Heatmap(
        z=h.values, x=[f"{x:02d}" for x in h.columns], y=labels,
        colorscale=scale, xgap=2, ygap=2, hoverongaps=False,
        colorbar=dict(title=dict(text="BLE max", font=dict(color=C["ink2"])),
                      tickfont=dict(color=C["muted"]), outlinewidth=0),
        hovertemplate="%{y} %{x}:00 Uhr – max. %{z} Geräte<extra></extra>"))
    base_layout(fig, max(220, 60 + 34 * len(days)))
    fig.update_layout(hovermode="closest")
    fig.update_xaxes(title_text="Stunde")
    fig.update_yaxes(autorange="reversed", gridcolor=C["surface"], rangemode=None)
    return fig


def chart_box(peaks: pd.DataFrame, days: list[date]) -> go.Figure:
    fig = go.Figure()
    for d in days:
        p = peaks[peaks["date"] == d]
        fig.add_trace(go.Box(
            y=p["BLE"], name=f"{WEEKDAYS[d.weekday()]} {d:%d.%m.}",
            marker=dict(color=C["ble"], size=4), line=dict(color=C["ble"], width=1.5),
            fillcolor="rgba(42,120,214,0.15)", boxpoints="outliers", showlegend=False))
    base_layout(fig, 340)
    fig.update_layout(hovermode="closest")
    fig.update_yaxes(title_text="BLE-Geräte (10-Min-Peak)")
    return fig


# ---------------------------------------------------------------------------
# HTML-Bericht
# ---------------------------------------------------------------------------
CSS = f"""
:root {{ color-scheme: light; }}
body {{ margin:0; background:{C['page']}; color:{C['ink']}; font-family:{FONT}; }}
main {{ max-width:1100px; margin:0 auto; padding:24px 16px 48px; }}
h1 {{ font-size:22px; margin:0 0 4px; }}
h2 {{ font-size:17px; margin:32px 0 6px; }}
p.sub, p.hint {{ color:{C['ink2']}; margin:0 0 12px; font-size:14px; }}
.card {{ background:{C['surface']}; border:1px solid rgba(11,11,11,.10);
        border-radius:10px; padding:12px; overflow-x:auto; }}
.tiles {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
         gap:12px; margin:16px 0; }}
.tile {{ background:{C['surface']}; border:1px solid rgba(11,11,11,.10);
        border-radius:10px; padding:12px 14px; }}
.tile .k {{ color:{C['ink2']}; font-size:13px; }}
.tile .v {{ font-size:28px; font-weight:600; margin-top:2px; }}
.tile .s {{ color:{C['muted']}; font-size:13px; }}
table {{ border-collapse:collapse; width:100%; font-size:13px;
        font-variant-numeric:tabular-nums; }}
th, td {{ padding:6px 8px; border-bottom:1px solid {C['grid']}; text-align:right;
         white-space:nowrap; }}
th {{ color:{C['ink2']}; font-weight:600; }}
th:first-child, td:first-child, th:nth-child(2), td:nth-child(2) {{ text-align:left; }}
ul.notes {{ color:{C['ink2']}; font-size:13px; }}
"""


def tile(k: str, v: str, s: str = "") -> str:
    return (f'<div class="tile"><div class="k">{html.escape(k)}</div>'
            f'<div class="v">{html.escape(v)}</div><div class="s">{html.escape(s)}</div></div>')


def build_report(raw, peaks, table, notes, folder: Path) -> str:
    days = list(table["Datum"])
    best = table.loc[table["BLE_Peak"].idxmax()]
    figs = [
        ("Tagesverlauf",
         "Dünn: Rohwert pro Messung · kräftig: 10-Minuten-Peak · blaues Band: "
         f"Peak-Phase (≥ {PEAK_THRESHOLD:.0%} des Tagesmaximums)",
         chart_days(raw, peaks, days)),
        ("Tagesvergleich", "BLE-10-Minuten-Peaks aller Tage auf einer Uhrzeit-Achse",
         chart_compare(peaks, days)),
        ("Stundenraster", "Höchster BLE-Wert je Stunde und Tag", chart_heatmap(raw, days)),
        ("Verteilung", "Spannweite der 10-Minuten-Peaks je Tag", chart_box(peaks, days)),
    ]
    parts = []
    for i, (title, sub, fig) in enumerate(figs):
        div = fig.to_html(full_html=False, include_plotlyjs="inline" if i == 0 else False,
                          config={"displaylogo": False, "locale": "de"})
        parts.append(f'<h2>{title}</h2><p class="sub">{html.escape(sub)}</p>'
                     f'<div class="card">{div}</div>')

    cols = ["Datum", "Tag", "Messung_von", "Messung_bis", "BLE_Peak", "BLE_Peak_Zeit",
            "BLE_Mittel", "BLE_Peakdauer_min", "WiFi_Peak", "WiFi_Mittel"]
    head = ["Datum", "Tag", "von", "bis", "BLE Peak", "um", "BLE Ø",
            "Peak-Dauer (min)", "WiFi Peak", "WiFi Ø"]
    trs = "".join(
        "<tr>" + "".join(f"<td>{html.escape(str(r[c] if c != 'Datum' else r[c].strftime('%d.%m.%Y')))}</td>"
                         for c in cols) + "</tr>"
        for _, r in table.iterrows())
    tbl = (f'<div class="card"><table><thead><tr>{"".join(f"<th>{h}</th>" for h in head)}'
           f"</tr></thead><tbody>{trs}</tbody></table></div>")

    wifi_hint = ""
    if (table["WiFi_Peak"] == 0).any():
        wifi_hint = ('<p class="hint">WiFi = 0 an einzelnen Tagen: Bei Firmware vor v1.2 wurde '
                     "der WiFi-Zähler nach dem NTP-Sync abgeschaltet.</p>")

    tiles = "".join([
        tile("Höchster Tagespeak (BLE)", f"{int(best['BLE_Peak'])}",
             f"{best['Datum']:%d.%m.%Y} um {best['BLE_Peak_Zeit']} Uhr"),
        tile("Ø Tagespeak (BLE)", f"{table['BLE_Peak'].mean():.0f}", f"über {len(table)} Tage"),
        tile("Ø Peak-Dauer", f"{table['BLE_Peakdauer_min'].mean():.0f} min",
             f"≥ {PEAK_THRESHOLD:.0%} des Tagesmax"),
        tile("Messwerte", f"{len(raw):,}".replace(",", "."),
             f"{raw['ts'].min():%d.%m.%Y} – {raw['ts'].max():%d.%m.%Y}"),
    ])
    note_html = ("<h2>Hinweise zum Einlesen</h2><ul class='notes'>"
                 + "".join(f"<li>{html.escape(n)}</li>" for n in notes) + "</ul>") if notes else ""

    return f"""<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Paxcounter-Auswertung</title><style>{CSS}</style></head>
<body><main>
<h1>Paxcounter-Auswertung</h1>
<p class="sub">Quelle: {html.escape(str(folder))} · Zeiten in MEZ/MESZ ·
BLE ist die Hauptkennzahl; Werte sind Geräte in Reichweite, keine exakten Personenzahlen.</p>
<div class="tiles">{tiles}</div>
<h2>Tage</h2>{wifi_hint}{tbl}
{''.join(parts)}
{note_html}
</main></body></html>"""


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Paxcounter-Auswertung (SD-Karten-CSV -> HTML-Bericht)")
    ap.add_argument("ordner", nargs="?", default=".", help="Ordner mit raw_*.csv (z.B. SD-Karte)")
    ap.add_argument("-o", "--out", help="Ausgabedatei (Standard: <ordner>/paxcounter_bericht.html)")
    ap.add_argument("--von", type=date.fromisoformat, help="erster Tag, z.B. 2026-11-06")
    ap.add_argument("--bis", type=date.fromisoformat, help="letzter Tag")
    ap.add_argument("--min-werte", type=int, default=10,
                    help="Tage mit weniger Messwerten ignorieren (Standard 10)")
    a = ap.parse_args()

    folder = Path(a.ordner).expanduser()
    if not folder.is_dir():
        sys.exit(f"Ordner nicht gefunden: {folder}")

    raw, notes = load_raw(folder)
    if a.von:
        raw = raw[raw["date"] >= a.von]
    if a.bis:
        raw = raw[raw["date"] <= a.bis]
    counts = raw.groupby("date").size()
    small = counts[counts < a.min_werte].index
    if len(small):
        notes.append("Ignoriert (zu wenige Messwerte): "
                     + ", ".join(f"{d:%d.%m.%Y}" for d in small))
        raw = raw[~raw["date"].isin(small)]
    if raw.empty:
        sys.exit(f"Keine auswertbaren raw_*.csv in {folder}")

    peaks = interval_peaks(raw)
    table = daily_table(raw, peaks)

    out = Path(a.out) if a.out else folder / "paxcounter_bericht.html"
    out.write_text(build_report(raw, peaks, table, notes, folder), encoding="utf-8")
    csv_out = out.with_name(out.stem + "_tage.csv")
    table.to_csv(csv_out, index=False, sep=";", decimal=",", encoding="utf-8-sig")

    print(table.to_string(index=False))
    for n in notes:
        print("Hinweis:", n)
    print(f"\nBericht: {out}\nTabelle: {csv_out}  (Excel-tauglich)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
