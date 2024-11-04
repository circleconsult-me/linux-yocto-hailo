/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This header provides constants for Hailo's Cadence torrent wrapper.
 */

#ifndef _DT_BINDINGS_HAILO_TORRENT_H
#define _DT_BINDINGS_HAILO_TORRENT_H


/*! 1x4 configuration (i.e. single 4 lane link with lane 0 as the master lane) */
#define PCI_PHY_LINK_LANES_CFG__1x4__MASTER_LANES_LN0 0
/*! 1x3+1x1 configuration (with lane 0 and 1 master lanes) */
#define PCI_PHY_LINK_LANES_CFG__1x3_PLUS_1x1__MASTER_LANES_LN0_LN1 1
/*! 2x2 configuration (i.e. 2 2-lane links with lane 0 and lane 2 as the master lanes) */
#define PCI_PHY_LINK_LANES_CFG__2x2__MASTER_LANES_LN0_LN2 2
/*! 2x1+1x2 configuration (i.e. 2 single lane links plus 1 2-lane link with lanes 0, 1 and 2 as the master lanes) */
#define PCI_PHY_LINK_LANES_CFG__2x1_PLUS_1x2__MASTER_LANES_LN0_LN1_LN2 3
/*! 1x3+1x1 configuration (i.e. 1 3-lane link plus 1 single lane link with lane 0 and 3 as master lanes) */
#define PCI_PHY_LINK_LANES_CFG__1x3_PLUS_1x1__MASTER_LANES_LN0_LN3 4
/* 2x1+1x2 configuration (with lane 0, 1 and 3 as master lanes) */
#define PCI_PHY_LINK_LANES_CFG__2x1_PLUS_1x2__MASTER_LANES_LN0_LN1_LN3 5
/*! 2x1+1x2 configuration (with lane 0, 2 and 3 as master lanes) */
#define PCI_PHY_LINK_LANES_CFG__2x1_PLUS_1x2__MASTER_LANES_LN0_LN2_LN3 6
/*! 4x1 configuration (i.e. 4 independent links) */
#define PCI_PHY_LINK_LANES_CFG__4x1__MASTER_LANES_LN0_LN1_LN2_LN4 7


#define PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_1 0
#define PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_2 1
#define PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_4 2
#define PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_8 3

#endif /* _DT_BINDINGS_HAILO_TORRENT_H */
