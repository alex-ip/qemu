/*
 * QEMU LASI PS/2 emulation
 *
 * Copyright (c) 2019 Sven Schnelle
 *
 */
#ifndef HW_INPUT_LASIPS2_H
#define HW_INPUT_LASIPS2_H

#include "exec/hwaddr.h"

typedef struct LASIPS2State LASIPS2State;

struct LASIPS2Port {
    SysBusDevice parent_obj;

    LASIPS2State *parent;
    MemoryRegion reg;
    void *dev;
    uint8_t id;
    uint8_t control;
    uint8_t buf;
    bool loopback_rbne;
    bool irq;
};

#define TYPE_LASIPS2_PORT "lasips2-port"
OBJECT_DECLARE_SIMPLE_TYPE(LASIPS2Port, LASIPS2_PORT)

struct LASIPS2KbdPort {
    LASIPS2Port parent_obj;
};

#define TYPE_LASIPS2_KBD_PORT "lasips2-kbd-port"
OBJECT_DECLARE_SIMPLE_TYPE(LASIPS2KbdPort, LASIPS2_KBD_PORT)

struct LASIPS2MousePort {
    LASIPS2Port parent_obj;
};

#define TYPE_LASIPS2_MOUSE_PORT "lasips2-mouse-port"
OBJECT_DECLARE_SIMPLE_TYPE(LASIPS2MousePort, LASIPS2_MOUSE_PORT)

struct LASIPS2State {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    LASIPS2Port kbd;
    LASIPS2Port mouse;
    qemu_irq irq;
};

#define TYPE_LASIPS2 "lasips2"
OBJECT_DECLARE_SIMPLE_TYPE(LASIPS2State, LASIPS2)

#endif /* HW_INPUT_LASIPS2_H */
