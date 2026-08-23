/*
 * QEMU ADB mouse support
 *
 * Copyright (c) 2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "hw/core/qdev-properties.h"
#include "hw/input/adb.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "adb-internal.h"
#include "trace.h"
#include "qom/object.h"

OBJECT_DECLARE_TYPE(MouseState, ADBMouseClass, ADB_MOUSE)

struct MouseState {
    /*< public >*/
    ADBDevice parent_obj;
    /*< private >*/

    QemuInputHandlerState *hs;
    int buttons_state, last_buttons_state;
    int dx, dy, dz;
    bool polled_once;
    /*
     * Multiplier applied to incoming host motion deltas. Host input
     * arrives in host pixels (~100 per inch of physical mouse travel)
     * while the classic Apple mouse protocol handler declares 200
     * counts per inch, so guests that normalize pointer speed by the
     * declared resolution (Mac OS X's AppleADBMouse does; classic Mac
     * OS's cursor scaling largely doesn't) see half-speed motion at
     * the default of 1. Set 2 for a faithful "200 cpi device" feel on
     * such guests: -global adb-mouse.motion-scale=2.
     */
    uint8_t motion_scale;
    /*
     * Offer the Extended Apple Mouse Protocol (handler 4) when a guest
     * asks for it. This affects pointer speed *scaling* only: Mac OS X
     * scales by the resolution we declare in register 1, where the
     * classic protocol's implied 200 cpi halves its pointer speed given
     * our counts are host pixels.
     *
     * Classic Mac OS does NOT work fine on it -- it silently negotiates
     * handler 4 on its own initiative (regardless of the post-reset
     * default-handler property) and then builds its pointer-acceleration
     * table from a completely different, degenerate code path: live
     * table dumps showed a real classic-protocol (handler 2) guest gets
     * a proper multi-segment ballistic curve, while the SAME guest under
     * handler 4 collapses to one linear segment plus a flat-output
     * terminator, capping the pointer at a fixed ~1.07 px per ADB poll
     * (~90 px/s at the standard 11 ms autopoll rate) no matter how the
     * declared resolution or our motion-scale property are tuned --
     * confirmed mathematically (threshold and slope both scale with
     * declared resolution, but their product, the terminator's flat
     * output, does not) and empirically (doubling declared resolution
     * left real injected motion just as capped; disabling the extended
     * protocol instead made the same motion track proportionally,
     * confirmed live on both a fresh scratch install and the user's own
     * long-used 9.2 disk, ruling out a missing-preferences explanation).
     * Default off so classic guests -- this machine's primary target --
     * get the real curve. Mac OS X guests may set
     * -global adb-mouse.extended-protocol=on, but do not require it:
     * this was long assumed to be mandatory for them, and that
     * assumption misdirected a later investigation into Mac OS X clicks
     * landing in the screen corner. The real cause was elsewhere (the
     * mach64 host-cursor-tracking workaround taking the motion stream
     * away from this device), and Mac OS X 10.2 was subsequently
     * verified working with this property left off.
     */
    bool extended_protocol;
    /*
     * Handler ID reported after reset. Real Apple ADB mice report 2
     * (Classic Apple Mouse Protocol, 200 counts per inch); 1 is the
     * 100 cpi variant. This value only matters while extended_protocol
     * is enabled -- with it off (the default) a guest can never
     * negotiate up to handler 4 regardless of where it started, and
     * live table dumps confirm 1 vs 2 makes no further difference to
     * the guest's resulting acceleration curve.
     *
     * Mac OS X wants 1: its AppleADBMouse Type 4 personality matches
     * "3-01" (address 3, default handler 1) and negotiates the
     * extended protocol from there; with 2 it stays on the classic
     * path and halves its pointer speed. That's the default here too,
     * even though classic Mac OS is this machine's primary guest --
     * see extended_protocol above for why classic guests don't need
     * this property touched at all.
     */
    uint8_t default_handler;
};


struct ADBMouseClass {
    /*< public >*/
    ADBDeviceClass parent_class;
    /*< private >*/

    DeviceRealize parent_realize;
};

#define ADB_MOUSE_BUTTON_LEFT   0x01
#define ADB_MOUSE_BUTTON_RIGHT  0x02

static void adb_mouse_handle_event(DeviceState *dev, QemuConsole *src,
                                   QemuInputEvent *evt)
{
    MouseState *s = (MouseState *)dev;
    static const int bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]   = ADB_MOUSE_BUTTON_LEFT,
        [INPUT_BUTTON_RIGHT]  = ADB_MOUSE_BUTTON_RIGHT,
    };

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        trace_adb_device_mouse_handle_event(evt->type, evt->rel.axis,
                                            evt->rel.value, 0);
        /*
         * Ignore implausibly large single-event deltas: display
         * backends synthesize window-sized relative jumps around
         * grab/ungrab and pointer-warp compensation (observed with
         * both SDL and Cocoa), which are not real hand motion --
         * integrating one teleports the guest cursor (observed as
         * occasional wild jumps while tracking in Mac OS X guests).
         * Real pointing devices deliver far smaller per-event deltas.
         */
        if (evt->rel.value > 256 || evt->rel.value < -256) {
            break;
        }
        if (evt->rel.axis == INPUT_AXIS_X) {
            s->dx += evt->rel.value * s->motion_scale;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            s->dy += evt->rel.value * s->motion_scale;
        }
        break;

    case INPUT_EVENT_KIND_BTN:
        trace_adb_device_mouse_handle_event(evt->type, evt->btn.button,
                                            0, evt->btn.down);
        if (bmap[evt->btn.button]) {
            if (evt->btn.down) {
                s->buttons_state |= bmap[evt->btn.button];
            } else {
                s->buttons_state &= ~bmap[evt->btn.button];
            }
        }
        break;

    default:
        /* keep gcc happy */
        break;
    }
}

static const QemuInputHandler adb_mouse_handler = {
    .name  = "QEMU ADB Mouse",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = adb_mouse_handle_event,
    /*
     * We do not need the .sync handler because unlike e.g. PS/2 where async
     * mouse events are sent over the serial port, an ADB mouse is constantly
     * polled by the host via the adb_mouse_poll() callback.
     */
};

static int adb_mouse_poll(ADBDevice *d, uint8_t *obuf)
{
    MouseState *s = ADB_MOUSE(d);
    int dx, dy;
    bool report_reset_transient = !s->polled_once;

    /*
     * A real ADB mouse reports its actual (idle) state the first time
     * it is talked to after a reset, even if the user has never touched
     * it -- that initial packet is what lets the ADB Manager clear its
     * low-memory MBState/mouse-tracking globals from their ROM-supplied
     * startup default. Suppressing it here (as "nothing changed yet")
     * left MBState stuck at its initial nonzero value forever, since we
     * never sent a single packet for the OS to update it from.
     */
    if (s->polled_once &&
        s->last_buttons_state == s->buttons_state &&
        s->dx == 0 && s->dy == 0) {
        return 0;
    }
    s->polled_once = true;

    dx = s->dx;
    if (dx < -63) {
        dx = -63;
    } else if (dx > 63) {
        dx = 63;
    }

    dy = s->dy;
    if (dy < -63) {
        dy = -63;
    } else if (dy > 63) {
        dy = 63;
    }

    /*
     * Report what accumulated since the last poll and DISCARD any
     * excess -- never carry a remainder into the next poll. A real ADB
     * mouse counts encoder pulses into a register that the host reads
     * and clears; it cannot bank motion it had no room to report, and
     * DingusPPC's AdbMouse::get_register_0() likewise zeroes x_rel/y_rel
     * after clamping.
     *
     * Subtracting only the reported amount (as this did) turns one fast
     * flick into a backlog delivered 63 counts at a time over many
     * polls. Two symptoms follow, both reported live: the pointer keeps
     * gliding for a second or more after the hand stops, and tracking
     * feels far too slow -- because the guest never sees a *fast*
     * delta, only a long stream of moderate ones, so classic Mac OS's
     * acceleration curve never engages and everything moves 1:1.
     * Raising motion-scale made it worse, which is the signature of a
     * backlog rather than a scaling problem.
     */
    /*
     * Report what accumulated since the last poll, keeping at most one
     * further poll's worth of overflow.
     *
     * Unbounded carry (the original `s->dx -= dx`) turned a single fast
     * flick into a backlog dribbled out 63 counts at a time over many
     * polls: the pointer kept gliding for a second after the hand
     * stopped, and the guest never saw a fast delta so its acceleration
     * curve never engaged. Discarding the overflow entirely (as
     * DingusPPC and real hardware do -- their counters are read-and-
     * cleared) removes the drift but throws away most of a fast flick,
     * because our deltas are host pixels already accelerated by the
     * host WM, which overshoot the classic protocol's +-63 far more
     * often than a real 200 cpi mouse ever would.
     *
     * Keeping a single poll of slack splits the difference: brief
     * bursts survive into the next poll (~20 ms later), while nothing
     * can accumulate into visible post-motion drift.
     */
    s->dx -= dx;
    s->dy -= dy;
    s->dx = MIN(MAX(s->dx, -63), 63);
    s->dy = MIN(MAX(s->dy, -63), 63);
    s->last_buttons_state = s->buttons_state;

    dx &= 0x7f;
    dy &= 0x7f;

    if (!(s->buttons_state & ADB_MOUSE_BUTTON_LEFT)) {
        dy |= 0x80;
    }
    if (!(s->buttons_state & ADB_MOUSE_BUTTON_RIGHT)) {
        dx |= 0x80;
    }

    if (report_reset_transient) {
        /*
         * Real ADB mice report a momentary button-down transient in their
         * very first post-reset Talk R0 response, even though the button
         * isn't physically pressed -- a side effect of real hardware's
         * power-on/reset behavior. Classic Mac OS's ADB Manager relies on
         * seeing a genuine transition (not just "first packet, still
         * idle") to clear the low-memory MBState global from its
         * ROM-supplied startup default: sending only a static idle
         * packet here (as the fix above does on its own) delivers data,
         * but never a *change*, so MBState never actually gets cleared.
         * Force that same one-shot transient here -- report the primary
         * (left) button as pressed on this call regardless of actual
         * state, and record a last_buttons_state that disagrees with the
         * real buttons_state so adb_mouse_has_data() schedules an
         * immediate follow-up poll, which will then correctly report the
         * real (not-pressed) state as a genuine release transition.
         */
        dy &= ~0x80;
        s->last_buttons_state = s->buttons_state ^ ADB_MOUSE_BUTTON_LEFT;
    }

    obuf[0] = dy;
    obuf[1] = dx;
    return 2;
}

static int adb_mouse_request(ADBDevice *d, uint8_t *obuf,
                             const uint8_t *buf, int len)
{
    MouseState *s = ADB_MOUSE(d);
    int cmd, reg, olen;

    if ((buf[0] & 0x0f) == ADB_FLUSH) {
        /*
         * Flush discards pending, not-yet-reported relative motion --
         * real hardware has no queued button data to flush, since the
         * button state is a live physical line, not buffered. Reverting
         * buttons_state to last_buttons_state here (as this code used to)
         * silently erases any real button transition (e.g. a release)
         * that happened between the last poll and this Flush, since
         * buttons_state is the only place that transition is recorded --
         * permanently desyncing the guest's button bookkeeping from
         * reality (observed as a window drag that never ends because the
         * guest never sees the mouse-up).
         */
        s->dx = 0;
        s->dy = 0;
        s->dz = 0;
        trace_adb_device_mouse_flush();
        return 0;
    }

    cmd = buf[0] & 0xc;
    reg = buf[0] & 0x3;
    olen = 0;
    switch (cmd) {
    case ADB_WRITEREG:
        trace_adb_device_mouse_writereg(reg, buf[1]);
        switch (reg) {
        case 2:
            break;
        case 3:
            /*
             * MacOS 9 has a bug in its ADB driver whereby after configuring
             * the ADB bus devices it sends another write of invalid length
             * to reg 3. Make sure we ignore it to prevent an address clash
             * with the previous device.
             */
            if (len != 3) {
                return 0;
            }

            switch (buf[2]) {
            case ADB_CMD_SELF_TEST:
                break;
            case ADB_CMD_CHANGE_ID:
            case ADB_CMD_CHANGE_ID_AND_ACT:
            case ADB_CMD_CHANGE_ID_AND_ENABLE:
                d->devaddr = buf[1] & 0xf;
                trace_adb_device_mouse_request_change_addr(d->devaddr);
                break;
            default:
                /*
                 * A plain handler assignment ignores the address field
                 * -- only the reserved handler bytes 0x00/0xFE/0xFD
                 * latch a new address (confirmed against DingusPPC's
                 * real-hardware-verified AdbMouse::set_register_3,
                 * which leaves my_addr untouched for handler values).
                 * Latching it here too (as this used to) would let a
                 * malformed handler-change write teleport the device
                 * to a different bus address.
                 *
                 * we support handlers:
                 * 0x01: Classic Apple Mouse Protocol / 100 cpi operations
                 * 0x02: Classic Apple Mouse Protocol / 200 cpi operations
                 * 0x04: Extended Apple Mouse Protocol (see the reg 1
                 *       read below -- lets the guest read our declared
                 *       resolution/button count instead of assuming the
                 *       classic protocol's fixed 200 cpi; Mac OS X's
                 *       AppleADBMouse scales pointer speed inversely
                 *       with device resolution, so the classic handler
                 *       made the cursor crawl at half speed given that
                 *       our "counts" are host pixels, not physical
                 *       1/200-inch mouse travel)
                 * we don't support handlers (at least):
                 * 0x03: Mouse systems A3 trackball
                 * 0x2f: Microspeed mouse
                 * 0x42: Macally
                 * 0x5f: Microspeed mouse
                 * 0x66: Microspeed mouse
                 */
                if (buf[2] == 1 || buf[2] == 2 ||
                    (buf[2] == 4 && s->extended_protocol)) {
                    d->handler = buf[2];
                }

                trace_adb_device_mouse_request_change_addr_and_handler(
                    d->devaddr, d->handler);
                break;
            }
        }
        break;
    case ADB_READREG:
        switch (reg) {
        case 0:
            olen = adb_mouse_poll(d, obuf);
            break;
        case 1:
            /*
             * Extended Apple Mouse Protocol device info -- must be
             * exactly 8 bytes or the guest rejects handler 4
             * (AppleADBMouseType4::probe): device signature (u32),
             * resolution in counts per inch (big-endian u16), device
             * class, button count. Resolution 100 gives a ~1:1 feel
             * since our motion units are host pixels (~100 per inch of
             * physical mouse travel), twice the speed the classic
             * handler's assumed 200 cpi produced. The signature must
             * not be 'tpad' (that selects the trackpad code path).
             */
            if (d->handler == 4) {
                /*
                 * Bytes 0-3 device identifier, 4-5 resolution in counts
                 * per inch (big-endian), 6 device class, 7 button count
                 * (byte roles confirmed against NetBSD's adbms_init_mouse(),
                 * which reads sc_class = buffer[6], sc_buttons = buffer[7]).
                 *
                 * The class byte selects how the driver INTERPRETS the
                 * motion data -- drivers keep separate classes for mouse,
                 * trackball, tablet and trackpad, and a tablet is decoded
                 * as absolute coordinates rather than relative deltas.
                 * This used to report 1 on the assumption that 1 meant
                 * "mouse"; 0 is the standard relative-mouse class (first
                 * in every driver's class enumeration), and reporting a
                 * non-mouse class is a plausible cause of a guest that
                 * honours our button bits while discarding our deltas.
                 *
                 * The identifier is what a real Apple mouse reports:
                 * drivers match known identifiers to pick a decode path,
                 * and an unknown one risks a quirk path. It must not be
                 * 'tpad' -- that selects the trackpad code.
                 *
                 * Resolution stays 100: our counts are host pixels
                 * (~100 per inch of physical travel), and guests that
                 * normalise pointer speed by this value -- Mac OS X's
                 * AppleADBMouse does -- then track 1:1.
                 */
                obuf[0] = 'a';
                obuf[1] = 'p';
                obuf[2] = 'p';
                obuf[3] = 'l';
                obuf[4] = 0;
                obuf[5] = 100;  /* counts per inch */
                obuf[6] = 0;    /* device class: relative mouse */
                obuf[7] = 2;    /* buttons */
                olen = 8;
                trace_adb_device_mouse_extended_info(obuf[5], obuf[6],
                                                     obuf[7]);
            }
            break;
        case 3:
            obuf[0] = d->devaddr;
            obuf[1] = d->handler;
            olen = 2;
            break;
        }
        if (olen > 0) {
            trace_adb_device_mouse_readreg(reg, obuf[0], obuf[1]);
        }
        break;
    }
    return olen;
}

static bool adb_mouse_has_data(ADBDevice *d)
{
    MouseState *s = ADB_MOUSE(d);

    return !s->polled_once ||
           !(s->last_buttons_state == s->buttons_state &&
             s->dx == 0 && s->dy == 0);
}

static void adb_mouse_reset(DeviceState *dev)
{
    ADBDevice *d = ADB_DEVICE(dev);
    MouseState *s = ADB_MOUSE(dev);

    /*
     * The default (reset-time) handler ID doubles as an IOKit driver
     * match key on Mac OS X: AppleADBMouse's Type 4 (Extended Apple
     * Mouse Protocol) personality matches "ADB Match" = "3-01" --
     * address 3 with DEFAULT handler 1 -- while the classic Type 1/2
     * personalities match bare address "3". With any other default
     * (this used to be 2, briefly 4) the extended-protocol driver
     * never even probes, OS X falls back to the classic 200 cpi
     * protocol, and pointer speed is halved (its HID layer normalizes
     * by declared resolution; our "counts" are host pixels at roughly
     * 100/inch). Defaulting to 1 lets Type 4 match, negotiate handler
     * 4, and read our true resolution from register 1. Guests that
     * only know the classic protocol simply run us as the 100 cpi
     * classic mouse, which matches our units anyway.
     */
    d->handler = s->default_handler;
    d->devaddr = ADB_DEVID_MOUSE;
    s->last_buttons_state = s->buttons_state = 0;
    s->dx = s->dy = s->dz = 0;
    s->polled_once = false;
}

static const VMStateDescription vmstate_adb_mouse = {
    .name = "adb_mouse",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, MouseState, 0, vmstate_adb_device,
                       ADBDevice),
        VMSTATE_INT32(buttons_state, MouseState),
        VMSTATE_INT32(last_buttons_state, MouseState),
        VMSTATE_INT32(dx, MouseState),
        VMSTATE_INT32(dy, MouseState),
        VMSTATE_INT32(dz, MouseState),
        VMSTATE_BOOL(polled_once, MouseState),
        VMSTATE_END_OF_LIST()
    }
};

static void adb_mouse_realizefn(DeviceState *dev, Error **errp)
{
    MouseState *s = ADB_MOUSE(dev);
    ADBMouseClass *amc = ADB_MOUSE_GET_CLASS(dev);

    amc->parent_realize(dev, errp);

    s->hs = qemu_input_handler_register(dev, &adb_mouse_handler);
}

static void adb_mouse_initfn(Object *obj)
{
    ADBDevice *d = ADB_DEVICE(obj);

    d->devaddr = ADB_DEVID_MOUSE;
}

static const Property adb_mouse_properties[] = {
    DEFINE_PROP_BOOL("extended-protocol", MouseState, extended_protocol, false),
    DEFINE_PROP_UINT8("default-handler", MouseState, default_handler, 1),
    DEFINE_PROP_UINT8("motion-scale", MouseState, motion_scale, 1),
};

static void adb_mouse_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ADBDeviceClass *adc = ADB_DEVICE_CLASS(oc);
    ADBMouseClass *amc = ADB_MOUSE_CLASS(oc);

    device_class_set_parent_realize(dc, adb_mouse_realizefn,
                                    &amc->parent_realize);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    adc->devreq = adb_mouse_request;
    adc->devhasdata = adb_mouse_has_data;
    device_class_set_legacy_reset(dc, adb_mouse_reset);
    dc->vmsd = &vmstate_adb_mouse;
    device_class_set_props(dc, adb_mouse_properties);
}

static const TypeInfo adb_mouse_type_info = {
    .name = TYPE_ADB_MOUSE,
    .parent = TYPE_ADB_DEVICE,
    .instance_size = sizeof(MouseState),
    .instance_init = adb_mouse_initfn,
    .class_init = adb_mouse_class_init,
    .class_size = sizeof(ADBMouseClass),
};

static void adb_mouse_register_types(void)
{
    type_register_static(&adb_mouse_type_info);
}

type_init(adb_mouse_register_types)
