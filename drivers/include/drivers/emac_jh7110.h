#ifndef DRIVERS_EMAC_JH7110_H
#define DRIVERS_EMAC_JH7110_H

#include <stdint.h>
#include <stdbool.h>

/* StarFive JH7110 Synopsys DesignWare Ethernet QoS v5.20 (DWC_ether_qos)
 * Gigabit Ethernet MAC driver (`gmac1@16040000` and `gmac0@16030000`) and
 * Motorcomm YT8531C RGMII-ID PHY driver for StarFive VisionFive 2.
 *
 * Exposes the same diagnostic and netif_t surface as drivers/emac_esp32p4.h
 * so shell commands (`emac scan`, `emac link`, `emac linktest`, `emac loopback`,
 * `emac stats`, `emac promisc`) and kernel/board.c work identically.
 */

struct netif;

typedef struct {
    bool     up;            /* carrier present */
    bool     an_complete;   /* auto-negotiation finished */
    uint16_t speed_mbit;    /* 10, 100, or 1000; meaningless when !up */
    bool     full_duplex;
} emac_link_t;

int  emac_probe(void);
int  emac_mdio_read(uint8_t phy_addr, uint8_t reg);
int  emac_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val);
void emac_phy_scan(void);

void emac_loopback_test(void);
void emac_loopback_stress(uint32_t rounds);
void emac_loopback_phy(uint32_t rounds);

int  emac_phy_autoneg_start(void);
bool emac_link_poll(emac_link_t *out);
void emac_link_report(void);
void emac_link_updown_test(void);

int           emac_netif_init(void);
struct netif *emac_get_netif(void);
void          emac_stats_report(void);
void          emac_set_promiscuous(bool on);

#endif /* DRIVERS_EMAC_JH7110_H */
