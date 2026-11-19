package com.zago.multiscreen;

import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;

/** Converte tocchi e penna del tablet in pacchetti per il PC. */
final class InputHandler {
    interface Sink {
        void send(byte[] pkt);
        void togglePanel();
    }

    static final int MODE_TOUCH = 0;
    static final int MODE_MOUSE = 1;

    private final Sink sink;
    private final float density;
    private int mode = MODE_TOUCH;
    private final Handler handler = new Handler(Looper.getMainLooper());

    // stato "modalita' mouse"
    private float downX, downY, lastCx, lastCy, scrollAccX, scrollAccY;
    private long downTime, twoDownTime;
    private boolean moved, buttonDown, longPressed, twoFinger, twoMoved, suppressed;
    private int lastNx, lastNy;

    InputHandler(Sink sink, float density) {
        this.sink = sink;
        this.density = density;
    }

    void setMode(int m) {
        mode = m;
    }

    boolean onTouch(View v, MotionEvent e) {
        int w = v.getWidth(), h = v.getHeight();
        int am = e.getActionMasked();

        // 4 dita = apre/chiude il pannello (non arriva al PC)
        if (am == MotionEvent.ACTION_POINTER_DOWN && e.getPointerCount() >= 4 && !suppressed) {
            suppressed = true;
            sink.send(Proto.packet(Proto.TOUCH_CANCEL, null));
            if (buttonDown) pointer(0, lastNx, lastNy);
            buttonDown = false;
            handler.removeCallbacks(longPress);
            sink.togglePanel();
            return true;
        }
        if (suppressed) {
            if (am == MotionEvent.ACTION_UP || am == MotionEvent.ACTION_CANCEL) suppressed = false;
            return true;
        }

        int tool = e.getToolType(e.getActionIndex());
        if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) {
            pen(e, w, h);
            return true;
        }
        if (tool == MotionEvent.TOOL_TYPE_MOUSE) {
            mouseDevice(e, w, h);
            return true;
        }
        if (mode == MODE_MOUSE) mouseGesture(e, w, h);
        else touch(e, w, h);
        return true;
    }

    /** Hover della penna / mouse collegato al tablet. */
    boolean onGeneric(View v, MotionEvent e) {
        int am = e.getActionMasked();
        int w = v.getWidth(), h = v.getHeight();
        if (am == MotionEvent.ACTION_HOVER_MOVE || am == MotionEvent.ACTION_HOVER_ENTER) {
            pointer(0, Proto.norm(e.getX(), w), Proto.norm(e.getY(), h));
            return true;
        }
        if (am == MotionEvent.ACTION_SCROLL) {
            float vs = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
            float hs = e.getAxisValue(MotionEvent.AXIS_HSCROLL);
            scroll(-hs * 15f, -vs * 15f);
            return true;
        }
        return false;
    }

    // ---------------- touch nativo ----------------
    private void touch(MotionEvent e, int w, int h) {
        int am = e.getActionMasked();
        switch (am) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                byte[] p = touchPacket(1);
                putTouch(p, 0, e.getPointerId(i), 0, Proto.norm(e.getX(i), w), Proto.norm(e.getY(i), h));
                sink.send(p);
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                int n = Math.min(e.getPointerCount(), 20);
                byte[] p = touchPacket(n);
                for (int i = 0; i < n; i++) {
                    putTouch(p, i, e.getPointerId(i), 1, Proto.norm(e.getX(i), w), Proto.norm(e.getY(i), h));
                }
                sink.send(p);
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                byte[] p = touchPacket(1);
                putTouch(p, 0, e.getPointerId(i), 2, Proto.norm(e.getX(i), w), Proto.norm(e.getY(i), h));
                sink.send(p);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                sink.send(Proto.packet(Proto.TOUCH_CANCEL, null));
                break;
            default:
                break;
        }
    }

    private static byte[] touchPacket(int n) {
        byte[] p = Proto.alloc(Proto.TOUCH, 1 + n * 6);
        p[5] = (byte) n;
        return p;
    }

    private static void putTouch(byte[] p, int slot, int id, int state, int x, int y) {
        int o = 6 + slot * 6;
        p[o] = (byte) id;
        p[o + 1] = (byte) state;
        Proto.put16(p, o + 2, x);
        Proto.put16(p, o + 4, y);
    }

    // ---------------- penna ----------------
    private void pen(MotionEvent e, int w, int h) {
        int am = e.getActionMasked();
        int nx = Proto.norm(e.getX(), w), ny = Proto.norm(e.getY(), h);
        boolean barrel = (e.getButtonState() & (MotionEvent.BUTTON_STYLUS_PRIMARY | MotionEvent.BUTTON_SECONDARY)) != 0;
        if (am == MotionEvent.ACTION_UP || am == MotionEvent.ACTION_CANCEL || am == MotionEvent.ACTION_POINTER_UP) {
            pointer(0, nx, ny);
        } else {
            pointer(barrel ? 2 : 1, nx, ny);
        }
    }

    // ---------------- mouse fisico collegato al tablet ----------------
    private void mouseDevice(MotionEvent e, int w, int h) {
        int b = e.getButtonState();
        int buttons = ((b & MotionEvent.BUTTON_PRIMARY) != 0 ? 1 : 0)
                | ((b & MotionEvent.BUTTON_SECONDARY) != 0 ? 2 : 0)
                | ((b & MotionEvent.BUTTON_TERTIARY) != 0 ? 4 : 0);
        int am = e.getActionMasked();
        if (am == MotionEvent.ACTION_UP || am == MotionEvent.ACTION_CANCEL) buttons = 0;
        pointer(buttons, Proto.norm(e.getX(), w), Proto.norm(e.getY(), h));
    }

    // ---------------- modalita' mouse (dito = mouse) ----------------
    private final Runnable longPress = new Runnable() {
        @Override
        public void run() {
            if (!moved && !twoFinger && !buttonDown) {
                longPressed = true;
                pointer(2, lastNx, lastNy);
                pointer(0, lastNx, lastNy);
            }
        }
    };

    private void mouseGesture(MotionEvent e, int w, int h) {
        int am = e.getActionMasked();
        float slop = 10 * density;
        switch (am) {
            case MotionEvent.ACTION_DOWN:
                downX = e.getX();
                downY = e.getY();
                downTime = e.getEventTime();
                moved = buttonDown = longPressed = twoFinger = twoMoved = false;
                lastNx = Proto.norm(downX, w);
                lastNy = Proto.norm(downY, h);
                pointer(0, lastNx, lastNy);
                handler.postDelayed(longPress, 550);
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                handler.removeCallbacks(longPress);
                if (buttonDown) {
                    pointer(0, lastNx, lastNy);
                    buttonDown = false;
                }
                if (e.getPointerCount() == 2) {
                    twoFinger = true;
                    twoMoved = false;
                    twoDownTime = e.getEventTime();
                    lastCx = (e.getX(0) + e.getX(1)) / 2f;
                    lastCy = (e.getY(0) + e.getY(1)) / 2f;
                    scrollAccX = scrollAccY = 0;
                }
                break;
            case MotionEvent.ACTION_MOVE:
                if (twoFinger) {
                    if (e.getPointerCount() < 2) break;
                    float cx = (e.getX(0) + e.getX(1)) / 2f, cy = (e.getY(0) + e.getY(1)) / 2f;
                    float dx = cx - lastCx, dy = cy - lastCy;
                    lastCx = cx;
                    lastCy = cy;
                    scrollAccX += dx;
                    scrollAccY += dy;
                    if (Math.abs(scrollAccX) + Math.abs(scrollAccY) > slop) twoMoved = true;
                    if (twoMoved) scroll(-dx / density, -dy / density);
                } else if (!longPressed) {
                    float x = e.getX(), y = e.getY();
                    if (!moved && Math.hypot(x - downX, y - downY) > slop) {
                        moved = true;
                        handler.removeCallbacks(longPress);
                        buttonDown = true;
                        pointer(1, Proto.norm(downX, w), Proto.norm(downY, h)); // inizia trascinamento dal punto iniziale
                    }
                    lastNx = Proto.norm(x, w);
                    lastNy = Proto.norm(y, h);
                    if (moved) pointer(1, lastNx, lastNy);
                }
                break;
            case MotionEvent.ACTION_POINTER_UP:
                if (twoFinger && e.getPointerCount() == 2 && !twoMoved && e.getEventTime() - twoDownTime < 350) {
                    pointer(2, lastNx, lastNy); // tap con due dita = click destro
                    pointer(0, lastNx, lastNy);
                    twoMoved = true; // non ripetere
                }
                break;
            case MotionEvent.ACTION_UP:
                handler.removeCallbacks(longPress);
                if (twoFinger || longPressed) break;
                lastNx = Proto.norm(e.getX(), w);
                lastNy = Proto.norm(e.getY(), h);
                if (buttonDown) {
                    pointer(0, lastNx, lastNy);
                    buttonDown = false;
                } else {
                    pointer(1, lastNx, lastNy); // tap = click
                    pointer(0, lastNx, lastNy);
                }
                break;
            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                if (buttonDown) pointer(0, lastNx, lastNy);
                buttonDown = false;
                break;
            default:
                break;
        }
    }

    private void pointer(int buttons, int nx, int ny) {
        byte[] p = Proto.alloc(Proto.POINTER, 5);
        p[5] = (byte) buttons;
        Proto.put16(p, 6, nx);
        Proto.put16(p, 8, ny);
        sink.send(p);
    }

    private void scroll(float dx, float dy) {
        byte[] p = Proto.alloc(Proto.SCROLL, 4);
        Proto.put16(p, 5, clamp16(Math.round(dx * 10)));
        Proto.put16(p, 7, clamp16(Math.round(dy * 10)));
        sink.send(p);
    }

    private static int clamp16(int v) {
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        return v & 0xffff;
    }
}
