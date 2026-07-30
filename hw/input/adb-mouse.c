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
        if (evt->rel.axis == INPUT_AXIS_X) {
            s->dx += evt->rel.value;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            s->dy += evt->rel.value;
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

    s->dx -= dx;
    s->dy -= dy;
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
                d->devaddr = buf[1] & 0xf;
                /*
                 * we support handlers:
                 * 0x01: Classic Apple Mouse Protocol / 100 cpi operations
                 * 0x02: Classic Apple Mouse Protocol / 200 cpi operations
                 * we don't support handlers (at least):
                 * 0x03: Mouse systems A3 trackball
                 * 0x04: Extended Apple Mouse Protocol
                 * 0x2f: Microspeed mouse
                 * 0x42: Macally
                 * 0x5f: Microspeed mouse
                 * 0x66: Microspeed mouse
                 */
                if (buf[2] == 1 || buf[2] == 2) {
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
            break;
        case 3:
            obuf[0] = d->devaddr;
            obuf[1] = d->handler;
            olen = 2;
            break;
        }
        trace_adb_device_mouse_readreg(reg, obuf[0], obuf[1]);
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

    d->handler = 2;
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
