package com.zago.multiscreen;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Piccoli helper per costruire l'interfaccia senza XML. */
final class Ui {
    static final int ACCENT = 0xFF4F8CFF;
    static final int BG = 0xFF10141C;
    static final int CARD = 0xFF1A2130;
    static final int TEXT = 0xFFDDE5F2;
    static final int MUTED = 0xFF8D99B0;

    interface OnChoice {
        void chosen(String key, String value);
    }

    private Ui() {}

    static int dp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }

    static TextView text(Context c, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    static LinearLayout card(Context c) {
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(c, 22), dp(c, 16), dp(c, 22), dp(c, 18));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(CARD);
        bg.setCornerRadius(dp(c, 18));
        card.setBackground(bg);
        return card;
    }

    static TextView button(Context c, String label, boolean primary, View.OnClickListener l) {
        TextView b = text(c, label, 15, primary ? Color.WHITE : TEXT, primary);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(c, 16), dp(c, 12), dp(c, 16), dp(c, 12));
        GradientDrawable d = new GradientDrawable();
        d.setCornerRadius(dp(c, 12));
        if (primary) d.setColor(ACCENT);
        else {
            d.setColor(0xFF242D40);
            d.setStroke(dp(c, 1), 0xFF35415A);
        }
        b.setBackground(d);
        b.setOnClickListener(l);
        b.setClickable(true);
        return b;
    }

    static LinearLayout row(Context c) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        return r;
    }

    static void addWeighted(LinearLayout row, View v, int marginLeftDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(dp(row.getContext(), marginLeftDp), 0, 0, 0);
        row.addView(v, lp);
    }

    /** Riga di "chip" a scelta singola. */
    static void option(final Context c, LinearLayout parent, String label, final String key, String current,
                       String[] labels, final String[] values, final OnChoice cb) {
        TextView l = text(c, label, 14, MUTED, false);
        l.setPadding(0, dp(c, 10), 0, dp(c, 6));
        parent.addView(l);
        final LinearLayout row = row(c);
        parent.addView(row);
        for (int i = 0; i < labels.length; i++) {
            TextView b = text(c, labels[i], 15, TEXT, false);
            b.setGravity(Gravity.CENTER);
            b.setPadding(dp(c, 6), dp(c, 10), dp(c, 6), dp(c, 10));
            b.setTag(values[i]);
            addWeighted(row, b, i == 0 ? 0 : 6);
            chip(c, b, values[i].equals(current));
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    String val = (String) v.getTag();
                    for (int j = 0; j < row.getChildCount(); j++) {
                        View ch = row.getChildAt(j);
                        chip(c, (TextView) ch, val.equals(ch.getTag()));
                    }
                    cb.chosen(key, val);
                }
            });
        }
    }

    static void chip(Context c, TextView b, boolean selected) {
        GradientDrawable d = new GradientDrawable();
        d.setCornerRadius(dp(c, 12));
        if (selected) {
            d.setColor(ACCENT);
            b.setTextColor(Color.WHITE);
        } else {
            d.setColor(0xFF242D40);
            d.setStroke(dp(c, 1), 0xFF35415A);
            b.setTextColor(0xFFB8C4D9);
        }
        b.setBackground(d);
    }
}
