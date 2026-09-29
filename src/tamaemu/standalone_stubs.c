/*
 * ESP-TamaCore standalone stubs.
 * P's standalone boot does not require desktop networking or PN512 NFC.
 * These symbols keep the original TamaEmu CPU/peripheral core linkable.
 */
#include "emu.h"

const char *disasm_sym_for(uint32_t pc)
{
    (void)pc;
    return "";
}

void pn512_power_on(Emu *e) { (void)e; }
void pn512_probe_set(Emu *e, int on) { (void)e; (void)on; }
void pn512_host_byte(Emu *e, uint8_t b) { (void)e; (void)b; }
void pn512_ca_tick(Emu *e) { (void)e; }

uint64_t link_now_us(void) { return 0; }
void link_set_tx_byte_us(Link *l, unsigned us) { (void)l; (void)us; }

void link_tx(Link *l, int from_core, uint8_t byte)
{
    (void)l;
    (void)from_core;
    (void)byte;
}

int link_rx_pending(Link *l, int for_core)
{
    (void)l;
    (void)for_core;
    return 0;
}

int link_rx(Link *l, int for_core)
{
    (void)l;
    (void)for_core;
    return -1;
}

void link_close(Link *l) { (void)l; }
int link_auto_poll(Link *l) { (void)l; return 0; }
void link_reset(Link *l) { (void)l; }

int link_auto_begin(Link *l, int port)
{
    (void)l;
    (void)port;
    return 0;
}

void link_ir_pc_sample(Emu *e) { (void)e; }
