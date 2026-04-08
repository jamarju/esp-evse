import marimo

__generated_with = "0.22.4"
app = marimo.App(width="medium")


@app.cell
def _():
    import altair as alt
    import marimo as mo
    import numpy as np
    import polars as pl

    return alt, mo, np, pl


@app.cell
def _(mo):
    mo.md(r"""
    # ESP-EVSE Ammeter Calibration

    1. In Home Assistant, find the number entities named `Current Scale Factor` and `Current Offset` for your charger and enter their current values below.
    2. Put a clamp meter around either live or neutral.
    3. Pick a few charging current setpoints across the usable range.
    4. After each change, wait for the current to settle and add one row with:
       - `reported_A`: the value from the `EV Charging Current` sensor in Home Assistant
       - `measured_A`: the current measured by the clamp

    More points usually give a better fit.
    The results and plot update automatically as you type.
    """)
    return


@app.cell
def _(mo):
    current_scale = mo.ui.number(
        value=220,
        label="Current Scale Factor (HA entity: Current Scale Factor)",
        start=1,
        stop=10000,
        step=1,
    )
    current_offset = mo.ui.number(
        value=0,
        label="Current Offset in mA (HA entity: Current Offset)",
        start=-10000,
        stop=10000,
        step=1,
    )
    mo.hstack([current_scale, current_offset], gap=2, justify="start")
    return current_offset, current_scale


@app.cell
def _(mo, pl):
    initial_measurements = pl.DataFrame(
        schema={
            "reported_A": pl.Float64,
            "measured_A": pl.Float64,
        }
    )
    editor = mo.ui.data_editor(
        data=initial_measurements,
        label="### Measurements",
    )
    editor
    return (editor,)


@app.cell
def _(alt, current_offset, current_scale, editor, mo, np, pl):
    filled_measurements = (
        pl.DataFrame(editor.value)
        .with_columns(
            pl.col("reported_A").cast(pl.Float64),
            pl.col("measured_A").cast(pl.Float64),
        )
        .filter(
            pl.col("reported_A").is_not_null(),
            pl.col("measured_A").is_not_null(),
        )
    )

    if len(filled_measurements) < 2:
        mo.stop(
            True,
            mo.md("Add at least two rows with both `reported_A` and `measured_A`."),
        )

    x = filled_measurements["reported_A"].to_numpy().astype(float)
    y = filled_measurements["measured_A"].to_numpy().astype(float)
    slope, intercept = np.polyfit(x, y, 1)

    scale_factor = round(current_scale.value * slope)
    offset = round(current_offset.value * slope - intercept * 1000)
    r2 = float(1 - np.sum((y - (slope * x + intercept)) ** 2) / np.sum((y - y.mean()) ** 2))

    regression_line = pl.DataFrame(
        {
            "reported_A": [
                float(filled_measurements["reported_A"].min()),
                float(filled_measurements["reported_A"].max()),
            ],
            "measured_A": [
                float(slope * filled_measurements["reported_A"].min() + intercept),
                float(slope * filled_measurements["reported_A"].max() + intercept),
            ],
        }
    )

    chart = (
        alt.Chart(regression_line)
        .mark_line(color="gray", strokeDash=[5, 5], opacity=0.6)
        .encode(
            x=alt.X("reported_A:Q", title="Reported current (A)"),
            y=alt.Y("measured_A:Q", title="Measured current (A)"),
        )
        + alt.Chart(filled_measurements)
        .mark_circle(size=90, color="steelblue")
        .encode(
            x=alt.X("reported_A:Q", title="Reported current (A)"),
            y=alt.Y("measured_A:Q", title="Measured current (A)"),
            tooltip=["reported_A", "measured_A"],
        )
    ).properties(width=560, height=360)

    highlighted_results = mo.md(
        f"""
        <div style="padding: 1rem 1.25rem; border: 2px solid #0891b2; border-radius: 0.75rem; background: #ecfeff;">
          <div style="font-size: 0.95rem; font-weight: 700; margin-bottom: 0.5rem;">Apply these values in Home Assistant</div>
          <div style="font-weight: 700; font-family: monospace; line-height: 1.5;">scale_factor = {scale_factor}</div>
          <div style="font-weight: 700; font-family: monospace; line-height: 1.5;">offset = {offset}</div>
        </div>
        """
    )

    mo.vstack(
        [
            highlighted_results,
            mo.md(
                f"""
                ## Results

                Based on the current values you entered above:

                - current `scale_factor = {current_scale.value}`
                - current `offset = {current_offset.value}`
                - new `scale_factor = {scale_factor}`
                - new `offset = {offset}`
                - `fit = measured = {slope:.4f} × reported + {intercept:.4f}`
                - `R² = {r2:.4f}`
                """
            ),
            mo.as_html(chart),
        ]
    )
    return


if __name__ == "__main__":
    app.run()
