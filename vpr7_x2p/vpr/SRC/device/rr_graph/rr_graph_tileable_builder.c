/**********************************************************
 * MIT License
 *
 * Copyright (c) 2018 LNIS - The University of Utah
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ***********************************************************************/

/************************************************************************
 * Filename:    rr_graph_tileable_builder.c
 * Created by:   Xifan Tang
 * Change history:
 * +-------------------------------------+
 * |  Date       |    Author   | Notes
 * +-------------------------------------+
 * | 2019/06/11  |  Xifan Tang | Created 
 * +-------------------------------------+
 ***********************************************************************/
/************************************************************************
 *  This file contains a builder for the complex rr_graph data structure 
 *  Different from VPR rr_graph builders, this builder aims to create a 
 *  highly regular rr_graph, where each Connection Block (CB), Switch 
 *  Block (SB) is the same (except for those on the borders). Thus, the
 *  rr_graph is called tileable, which brings significant advantage in 
 *  producing large FPGA fabrics.
 ***********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <vector>
#include <algorithm>

#include "vtr_ndmatrix.h"

#include "vpr_types.h"
#include "globals.h"
#include "vpr_utils.h"
#include "rr_graph_util.h"
#include "ReadOptions.h"
#include "rr_graph.h"
#include "rr_graph2.h"
#include "check_rr_graph.h"
#include "route_common.h"
#include "fpga_x2p_types.h"
#include "rr_graph_tileable_builder.h"

#include "rr_blocks.h"
#include "chan_node_details.h"
#include "device_coordinator.h"
#include "rr_graph_tileable_gsb.h"

/************************************************************************
 * Local data stuctures in the file 
 ***********************************************************************/

/************************************************************************
 * Local function in the file 
 ***********************************************************************/

/************************************************************************
 * Initialize a rr_node 
 ************************************************************************/
static 
void tileable_rr_graph_init_rr_node(t_rr_node* cur_rr_node) {
  cur_rr_node->xlow = 0;
  cur_rr_node->xhigh = 0;
  cur_rr_node->ylow = 0;
  cur_rr_node->xhigh = 0;

  cur_rr_node->ptc_num = 0; 
  cur_rr_node->track_ids.clear();

  cur_rr_node->cost_index = 0; 
  cur_rr_node->occ = 0; 
  cur_rr_node->fan_in = 0; 
  cur_rr_node->num_edges = 0; 
  cur_rr_node->type = NUM_RR_TYPES; 
  cur_rr_node->edges = NULL; 
  cur_rr_node->switches = NULL; 

  cur_rr_node->driver_switch = 0; 
  cur_rr_node->unbuf_switched = 0; 
  cur_rr_node->buffered = 0; 
  cur_rr_node->R = 0.; 
  cur_rr_node->C = 0.; 

  cur_rr_node->direction = BI_DIRECTION; /* Give an invalid value, easy to check errors */ 
  cur_rr_node->drivers = SINGLE; 
  cur_rr_node->num_wire_drivers = 0; 
  cur_rr_node->num_opin_drivers = 0; 

  cur_rr_node->num_drive_rr_nodes = 0; 
  cur_rr_node->drive_rr_nodes = NULL; 
  cur_rr_node->drive_switches = NULL; 

  cur_rr_node->vpack_net_num_changed = FALSE; 
  cur_rr_node->is_parasitic_net = FALSE; 
  cur_rr_node->is_in_heap = FALSE; 

  cur_rr_node->sb_num_drive_rr_nodes = 0; 
  cur_rr_node->sb_drive_rr_nodes = NULL; 
  cur_rr_node->sb_drive_switches = NULL; 

  cur_rr_node->pb = NULL; 

  cur_rr_node->name_mux = NULL; 
  cur_rr_node->id_path = -1; 

  cur_rr_node->prev_node = -1; 
  cur_rr_node->prev_edge = -1; 
  cur_rr_node->net_num = -1; 
  cur_rr_node->vpack_net_num = -1; 

  cur_rr_node->prev_node_in_pack = -1; 
  cur_rr_node->prev_edge_in_pack = -1; 
  cur_rr_node->net_num_in_pack = -1; 

  cur_rr_node->pb_graph_pin = NULL; 
  cur_rr_node->tnode = NULL; 
  
  cur_rr_node->pack_intrinsic_cost = 0.; 
  cur_rr_node->z = 0; 

  return;
}

/************************************************************************
 * Generate the number of tracks for each types of routing segments
 * w.r.t. the frequency of each of segments and channel width
 * Note that if we dertermine the number of tracks per type using
 *     chan_width * segment_frequency / total_freq may cause 
 * The total track num may not match the chan_width, 
 * therefore, we assign tracks one by one until we meet the frequency requirement
 * In this way, we can assign the number of tracks with repect to frequency 
 ***********************************************************************/
static 
std::vector<size_t> get_num_tracks_per_seg_type(const size_t chan_width, 
                                                const std::vector<t_segment_inf> segment_inf, 
                                                const bool use_full_seg_groups) {
  std::vector<size_t> result;
  std::vector<double> demand;
  /* Make sure a clean start */
  result.resize(segment_inf.size());
  demand.resize(segment_inf.size());

  /* Scale factor so we can divide by any length
   * and still use integers */
  /* Get the sum of frequency */
  size_t scale = 1;
  size_t freq_sum = 0;
  for (size_t iseg = 0; iseg < segment_inf.size(); ++iseg) {
    scale *= segment_inf[iseg].length;
    freq_sum += segment_inf[iseg].frequency;
  }
  size_t reduce = scale * freq_sum;

  /* Init assignments to 0 and set the demand values */
  /* Get the fraction of each segment type considering the frequency:
   * num_track_per_seg = chan_width * (freq_of_seg / sum_freq)
   */
  for (size_t iseg = 0; iseg < segment_inf.size(); ++iseg) {
    result[iseg] = 0;
    demand[iseg] = scale * chan_width * segment_inf[iseg].frequency;
    if (true == use_full_seg_groups) {
      demand[iseg] /= segment_inf[iseg].length;
    }
  }

  /* check if the sum of num_tracks, matches the chan_width */
  /* Keep assigning tracks until we use them up */
  size_t assigned = 0;
  size_t size = 0;
  size_t imax = 0;
  while (assigned < chan_width) {
    /* Find current maximum demand */
    double max = 0;
    for (size_t iseg = 0; iseg < segment_inf.size(); ++iseg) {
      if (demand[iseg] > max) {
        imax = iseg;
      }
      max = std::max(demand[iseg], max); 
    }

    /* Assign tracks to the type and reduce the types demand */
    size = (use_full_seg_groups ? segment_inf[imax].length : 1);
    demand[imax] -= reduce;
    result[imax] += size;
    assigned += size;
  }

  /* Undo last assignment if we were closer to goal without it */
  if ((assigned - chan_width) > (size / 2)) {
    result[imax] -= size;
  }

  return result;
} 

/************************************************************************
 * Build details of routing tracks in a channel 
 * The function will 
 * 1. Assign the segments for each routing channel,
 *    To be specific, for each routing track, we assign a routing segment.
 *    The assignment is subject to users' specifications, such as 
 *    a. length of each type of segment
 *    b. frequency of each type of segment.
 *    c. routing channel width
 *
 * 2. The starting point of each segment in the channel will be assigned
 *    For each segment group with same directionality (tracks have the same length),
 *    every L track will be a starting point (where L denotes the length of segments)
 *    In this case, if the number of tracks is not a multiple of L,
 *    indeed we may have some <L segments. This can be considered as a side effect.
 *    But still the rr_graph is tileable, which is the first concern!
 *
 *    Here is a quick example of Length-4 wires in a W=12 routing channel
 *    +---------------------------------------+--------------+
 *    | Index |   Direction  | Starting Point | Ending Point |
 *    +---------------------------------------+--------------+
 *    |   0   | MUX--------> |   Yes          |  No          |
 *    +---------------------------------------+--------------+
 *    |   1   | <--------MUX |   Yes          |  No          |
 *    +---------------------------------------+--------------+
 *    |   2   |   -------->  |   No           |  No          |
 *    +---------------------------------------+--------------+
 *    |   3   | <--------    |   No           |  No          |
 *    +---------------------------------------+--------------+
 *    |   4   |    --------> |   No           |  No          |
 *    +---------------------------------------+--------------+
 *    |   5   | <--------    |   No           |  No          |
 *    +---------------------------------------+--------------+
 *    |   7   | -------->MUX |   No           |  Yes         |
 *    +---------------------------------------+--------------+
 *    |   8   | MUX<-------- |   No           |  Yes         |
 *    +---------------------------------------+--------------+
 *    |   9   | MUX--------> |   Yes          |  No          |
 *    +---------------------------------------+--------------+
 *    |   10  | <--------MUX |   Yes          |  No          |
 *    +---------------------------------------+--------------+
 *    |   11  | --------> |   No        |
 *    +---------------------------------+
 *    |   12  | <-------- |   No        |
 *    +---------------------------------+
 *
 * 3. SPECIAL for fringes: TOP|RIGHT|BOTTOM|RIGHT
 *    if device_side is NUM_SIDES, we assume this channel does not locate on borders
 *    All segments will start and ends with no exception
 *
 * 4. IMPORTANT: we should be aware that channel width maybe different 
 *    in X-direction and Y-direction channels!!!
 *    So we will load segment details for different channels 
 ***********************************************************************/
ChanNodeDetails build_unidir_chan_node_details(const size_t chan_width, const size_t max_seg_length,
                                               const enum e_side device_side, 
                                               const std::vector<t_segment_inf> segment_inf) {
  ChanNodeDetails chan_node_details;
  size_t actual_chan_width = chan_width;
  /* Correct the chan_width: it should be an even number */
  if (0 != actual_chan_width % 2) {
    actual_chan_width++; /* increment it to be even */
  }
  assert (0 == actual_chan_width % 2);
  
  /* Reserve channel width */
  chan_node_details.reserve(chan_width);
  /* Return if zero width is forced */
  if (0 == actual_chan_width) {
    return chan_node_details; 
  }

  /* Find the number of segments required by each group */
  std::vector<size_t> num_tracks = get_num_tracks_per_seg_type(actual_chan_width/2, segment_inf, TRUE);  

  /* Add node to ChanNodeDetails */
  size_t cur_track = 0;
  for (size_t iseg = 0; iseg < segment_inf.size(); ++iseg) {
    /* segment length will be set to maxium segment length if this is a longwire */
    size_t seg_len = segment_inf[iseg].length;
    if (TRUE == segment_inf[iseg].longline) {
       seg_len = max_seg_length;
    } 
    for (size_t itrack = 0; itrack < num_tracks[iseg]; ++itrack) {
      bool seg_start = false;
      bool seg_end = false;
      /* Every first track of a group of Length-N wires, we set a starting point */
      if (0 == itrack % seg_len) {
        seg_start = true;
      }
      /* Every last track of a group of Length-N wires, we set an ending point */
      if (seg_len - 1 == itrack % seg_len) {
        seg_end = true;
      }
      /* Since this is a unidirectional routing architecture,
       * Add a pair of tracks, 1 INC_DIRECTION track and 1 DEC_DIRECTION track 
       */
      chan_node_details.add_track(cur_track, INC_DIRECTION, iseg, seg_len, seg_start, seg_end);
      cur_track++;
      chan_node_details.add_track(cur_track, DEC_DIRECTION, iseg, seg_len, seg_start, seg_end);
      cur_track++;
    }    
  }
  /* Check if all the tracks have been satisified */ 
  assert (cur_track == actual_chan_width);

  /* If this is on the border of a device, segments should start */
  switch (device_side) {
  case TOP:
  case RIGHT:
    /* INC_DIRECTION should all end */
    chan_node_details.set_tracks_end(INC_DIRECTION);
    /* DEC_DIRECTION should all start */
    chan_node_details.set_tracks_start(DEC_DIRECTION);
    break;
  case BOTTOM:
  case LEFT:
    /* INC_DIRECTION should all start */
    chan_node_details.set_tracks_start(INC_DIRECTION);
    /* DEC_DIRECTION should all end */
    chan_node_details.set_tracks_end(DEC_DIRECTION);
    break;
  case NUM_SIDES:
    break;
  default:
    vpr_printf(TIO_MESSAGE_ERROR, 
               "(File:%s, [LINE%d]) Invalid device_side!\n", 
               __FILE__, __LINE__);
    exit(1);
  }

  return chan_node_details; 
}

/* Deteremine the side of a io grid */
static 
enum e_side determine_io_grid_pin_side(const DeviceCoordinator& device_size, 
                                       const DeviceCoordinator& grid_coordinator) {
  /* TOP side IO of FPGA */
  if (device_size.get_y() == grid_coordinator.get_y()) {
    return BOTTOM; /* Such I/O has only Bottom side pins */
  } else if (device_size.get_x() == grid_coordinator.get_x()) { /* RIGHT side IO of FPGA */
    return LEFT; /* Such I/O has only Left side pins */
  } else if (0 == grid_coordinator.get_y()) { /* BOTTOM side IO of FPGA */
    return TOP; /* Such I/O has only Top side pins */
  } else if (0 == grid_coordinator.get_x()) { /* LEFT side IO of FPGA */
    return RIGHT; /* Such I/O has only Right side pins */
  } else {
    vpr_printf(TIO_MESSAGE_ERROR, 
               "(File:%s, [LINE%d]) I/O Grid is in the center part of FPGA! Currently unsupported!\n",
               __FILE__, __LINE__);
    exit(1);
  }
}

/************************************************************************
 * Get a list of pin_index for a grid (either OPIN or IPIN)
 * For IO_TYPE, only one side will be used, we consider one side of pins 
 * For others, we consider all the sides  
 ***********************************************************************/
static 
std::vector<int> get_grid_side_pins(const t_grid_tile& cur_grid, 
                                    const enum e_pin_type pin_type, 
                                    const enum e_side pin_side, 
                                    const int pin_height) {
  std::vector<int> pin_list; 
  /* Make sure a clear start */
  pin_list.clear();

  for (int ipin = 0; ipin < cur_grid.type->num_pins; ++ipin) {
    if ( (1 == cur_grid.type->pinloc[pin_height][pin_side][ipin]) 
      && (pin_type == cur_grid.type->pin_class[ipin]) ) {
      pin_list.push_back(ipin);
    }
  }
  return pin_list;
}

/************************************************************************
 * Get the number of pins for a grid (either OPIN or IPIN)
 * For IO_TYPE, only one side will be used, we consider one side of pins 
 * For others, we consider all the sides  
 ***********************************************************************/
static 
size_t get_grid_num_pins(const t_grid_tile& cur_grid, const enum e_pin_type pin_type, const enum e_side io_side) {
  size_t num_pins = 0;
  Side io_side_manager(io_side);
  /* For IO_TYPE sides */
  for (size_t side = 0; side < NUM_SIDES; ++side) {
    Side side_manager(side);
    /* skip unwanted sides */
    if ( (IO_TYPE == cur_grid.type)
      && (side != io_side_manager.to_size_t()) ) { 
      continue;
    }
    /* Get pin list */
    for (int height = 0; height < cur_grid.type->height; ++height) {
      std::vector<int> pin_list = get_grid_side_pins(cur_grid, pin_type, side_manager.get_side(), height);
      num_pins += pin_list.size();
    } 
  }

  return num_pins;
}

/************************************************************************
 * Estimate the number of rr_nodes per category:
 * CHANX, CHANY, IPIN, OPIN, SOURCE, SINK 
 ***********************************************************************/
static 
std::vector<size_t> estimate_num_rr_nodes_per_type(const DeviceCoordinator& device_size,
                                                   const std::vector<std::vector<t_grid_tile>> grids,
                                                   const std::vector<size_t> chan_width,
                                                   const std::vector<t_segment_inf> segment_infs) {
  std::vector<size_t> num_rr_nodes_per_type;
  /* reserve the vector: 
   * we have the follow type:
   * SOURCE = 0, SINK, IPIN, OPIN, CHANX, CHANY, INTRA_CLUSTER_EDGE, NUM_RR_TYPES
   * NUM_RR_TYPES and INTRA_CLUSTER_EDGE will be 0
   */
  num_rr_nodes_per_type.resize(NUM_RR_TYPES);
  /* Make sure a clean start */
  for (size_t i = 0; i < NUM_RR_TYPES; ++i) {
    num_rr_nodes_per_type[i] = 0;
  }

  /************************************************************************
   * 1. Search the grid and find the number OPINs and IPINs per grid
   *    Note that the number of SOURCE nodes are the same as OPINs
   *    and the number of SINK nodes are the same as IPINs
   ***********************************************************************/
  for (size_t ix = 0; ix < grids.size(); ++ix) {
    for (size_t iy = 0; iy < grids[ix].size(); ++iy) { 
      /* Skip EMPTY tiles */
      if (EMPTY_TYPE == grids[ix][iy].type) {
        continue;
      }
      /* Skip height>1 tiles (mostly heterogeneous blocks) */
      if (0 < grids[ix][iy].offset) {
        continue;
      }
      enum e_side io_side = NUM_SIDES;
      /* If this is the block on borders, we consider IO side */
      if (IO_TYPE == grid[ix][iy].type) {
        DeviceCoordinator io_device_size(device_size.get_x() - 1, device_size.get_y() - 1);
        DeviceCoordinator grid_coordinator(ix, iy);
        io_side = determine_io_grid_pin_side(io_device_size, grid_coordinator);
      }
      /* get the number of OPINs */
      num_rr_nodes_per_type[OPIN] += get_grid_num_pins(grids[ix][iy], DRIVER, io_side);
      /* get the number of IPINs */
      num_rr_nodes_per_type[IPIN] += get_grid_num_pins(grids[ix][iy], RECEIVER, io_side);
    }
  }
  /* SOURCE and SINK */
  num_rr_nodes_per_type[SOURCE] = num_rr_nodes_per_type[OPIN];
  num_rr_nodes_per_type[SINK]   = num_rr_nodes_per_type[IPIN];

  /************************************************************************
   * 2. Assign the segments for each routing channel,
   *    To be specific, for each routing track, we assign a routing segment.
   *    The assignment is subject to users' specifications, such as 
   *    a. length of each type of segment
   *    b. frequency of each type of segment.
   *    c. routing channel width
   *
   *    SPECIAL for fringes:
   *    All segments will start and ends with no exception
   *
   *    IMPORTANT: we should be aware that channel width maybe different 
   *    in X-direction and Y-direction channels!!!
   *    So we will load segment details for different channels 
   ***********************************************************************/
  /* For X-direction Channel: CHANX */
  for (size_t iy = 0; iy < device_size.get_y() - 1; ++iy) { 
    for (size_t ix = 1; ix < device_size.get_x() - 1; ++ix) {
      enum e_side chan_side = NUM_SIDES;
      /* For LEFT side of FPGA */
      if (0 == ix) {
        chan_side = LEFT;
      }
      /* For RIGHT side of FPGA */
      if (grids.size() - 2 == ix) {
        chan_side = RIGHT;
      }
      ChanNodeDetails chanx_details = build_unidir_chan_node_details(chan_width[0], device_size.get_x() - 2, chan_side, segment_infs); 
      num_rr_nodes_per_type[CHANX] += chanx_details.get_num_starting_tracks();
    }
  }

  /* For Y-direction Channel: CHANX */
  for (size_t ix = 0; ix < device_size.get_x() - 1; ++ix) {
    for (size_t iy = 1; iy < device_size.get_y() - 1; ++iy) { 
      enum e_side chan_side = NUM_SIDES;
      /* For LEFT side of FPGA */
      if (0 == iy) {
        chan_side = BOTTOM;
      }
      /* For RIGHT side of FPGA */
      if (grids[ix].size() - 2 == iy) {
        chan_side = TOP;
      }
      ChanNodeDetails chany_details = build_unidir_chan_node_details(chan_width[1], device_size.get_y() - 2, chan_side, segment_infs); 
      num_rr_nodes_per_type[CHANY] += chany_details.get_num_starting_tracks();
    }
  }

  return num_rr_nodes_per_type;
}

/************************************************************************
 * Configure one rr_node to the fast-look up of a rr_graph 
 ***********************************************************************/
static 
void load_one_node_to_rr_graph_fast_lookup(t_rr_graph* rr_graph, const int node_index,
                                           const t_rr_type node_type, 
                                           const int x, const int y, 
                                           const int ptc_num) {
  /* check the size of ivec (nelem), 
   * if the ptc_num exceeds the size limit, we realloc the ivec */
  if (ptc_num + 1 > rr_graph->rr_node_indices[node_type][x][y].nelem) {
    rr_graph->rr_node_indices[node_type][x][y].nelem = ptc_num + 1;
    rr_graph->rr_node_indices[node_type][x][y].list = (int*) my_realloc(rr_graph->rr_node_indices[node_type][x][y].list, sizeof(int) * (ptc_num + 1)); 
  }
  /* fill the lookup table */
  rr_graph->rr_node_indices[node_type][x][y].list[ptc_num] = node_index;
  return;
}

/************************************************************************
 * Configure rr_nodes for this grid 
 * coordinators: xlow, ylow, xhigh, yhigh, 
 * features: capacity, ptc_num (pin_num),
 ***********************************************************************/
static 
void load_one_grid_rr_nodes_basic_info(const DeviceCoordinator& grid_coordinator, 
                                       const t_grid_tile& cur_grid, 
                                       const enum e_side io_side, 
                                       t_rr_graph* rr_graph, 
                                       size_t* cur_node_id,
                                       const int wire_to_ipin_switch, const int delayless_switch) {
   Side io_side_manager(io_side);
  /* Walk through the height of each grid,
   * get pins and configure the rr_nodes */
  for (int height = 0; height < cur_grid.type->height; ++height) {
    /* Walk through sides */
    for (size_t side = 0; side < NUM_SIDES; ++side) {
      Side side_manager(side);
      /* skip unwanted sides */
      if ( (IO_TYPE == cur_grid.type)
        && (side != io_side_manager.to_size_t()) ) { 
        continue;
      }
      /* Find OPINs */
      /* Configure pins by pins */
      std::vector<int> opin_list = get_grid_side_pins(cur_grid, DRIVER, side_manager.get_side(), height);
      for (size_t pin = 0; pin < opin_list.size(); ++pin) {
        /* Configure the rr_node for the OPIN */
        rr_graph->rr_node[*cur_node_id].type  = OPIN; 
        rr_graph->rr_node[*cur_node_id].xlow  = grid_coordinator.get_x(); 
        rr_graph->rr_node[*cur_node_id].xhigh = grid_coordinator.get_x(); 
        rr_graph->rr_node[*cur_node_id].ylow  = grid_coordinator.get_y(); 
        rr_graph->rr_node[*cur_node_id].yhigh = grid_coordinator.get_y(); 
        rr_graph->rr_node[*cur_node_id].ptc_num  = opin_list[pin]; 
        rr_graph->rr_node[*cur_node_id].capacity = 1; 
        rr_graph->rr_node[*cur_node_id].occ = 0; 
        /* cost index is a FIXED value for OPIN */
        rr_graph->rr_node[*cur_node_id].cost_index = OPIN_COST_INDEX; 
        /* Switch info */
        rr_graph->rr_node[*cur_node_id].driver_switch = delayless_switch; 
        /* fill fast look-up table */
        load_one_node_to_rr_graph_fast_lookup(rr_graph, *cur_node_id, 
                                              rr_graph->rr_node[*cur_node_id].type, 
                                              rr_graph->rr_node[*cur_node_id].xlow, 
                                              rr_graph->rr_node[*cur_node_id].ylow,
                                              rr_graph->rr_node[*cur_node_id].ptc_num);
        /* Update node counter */
        (*cur_node_id)++;
      }
      /* Find IPINs */
      /* Configure pins by pins */
      std::vector<int> ipin_list = get_grid_side_pins(cur_grid, RECEIVER, side_manager.get_side(), height);
      for (size_t pin = 0; pin < ipin_list.size(); ++pin) {
        rr_graph->rr_node[*cur_node_id].type  = IPIN; 
        rr_graph->rr_node[*cur_node_id].xlow  = grid_coordinator.get_x(); 
        rr_graph->rr_node[*cur_node_id].xhigh = grid_coordinator.get_x(); 
        rr_graph->rr_node[*cur_node_id].ylow  = grid_coordinator.get_y(); 
        rr_graph->rr_node[*cur_node_id].yhigh = grid_coordinator.get_y(); 
        rr_graph->rr_node[*cur_node_id].ptc_num  = opin_list[pin]; 
        rr_graph->rr_node[*cur_node_id].capacity = 1; 
        rr_graph->rr_node[*cur_node_id].occ = 0; 
        /* cost index is a FIXED value for IPIN */
        rr_graph->rr_node[*cur_node_id].cost_index = IPIN_COST_INDEX; 
        /* Switch info */
        rr_graph->rr_node[*cur_node_id].driver_switch = wire_to_ipin_switch; 
        /* fill fast look-up table */
        load_one_node_to_rr_graph_fast_lookup(rr_graph, *cur_node_id, 
                                              rr_graph->rr_node[*cur_node_id].type, 
                                              rr_graph->rr_node[*cur_node_id].xlow, 
                                              rr_graph->rr_node[*cur_node_id].ylow,
                                              rr_graph->rr_node[*cur_node_id].ptc_num);
        /* Update node counter */
        (*cur_node_id)++;
      }
      /* Set a SOURCE or a SINK rr_node for each class */
      for (int iclass = 0; iclass < cur_grid.type->num_class; ++iclass) {
        /* Set a SINK rr_node for the OPIN */
        if ( DRIVER == cur_grid.type->class_inf[iclass].type) {
          rr_graph->rr_node[*cur_node_id].type  = SOURCE; 
        } 
        if ( RECEIVER == cur_grid.type->class_inf[iclass].type) {
          rr_graph->rr_node[*cur_node_id].type  = SINK; 
        }
        rr_graph->rr_node[*cur_node_id].xlow  = grid_coordinator.get_x(); 
        rr_graph->rr_node[*cur_node_id].xhigh = grid_coordinator.get_x(); 
        rr_graph->rr_node[*cur_node_id].ylow  = grid_coordinator.get_y(); 
        rr_graph->rr_node[*cur_node_id].yhigh = grid_coordinator.get_y(); 
        rr_graph->rr_node[*cur_node_id].ptc_num  = iclass; 
        /* FIXME: need to confirm if the capacity should be the number of pins in this class*/ 
        rr_graph->rr_node[*cur_node_id].capacity = cur_grid.type->class_inf[iclass].num_pins; 
        rr_graph->rr_node[*cur_node_id].occ = 0; 
        /* cost index is a FIXED value for SOURCE and SINK */
        if (SOURCE == rr_graph->rr_node[*cur_node_id].type) {
          rr_graph->rr_node[*cur_node_id].cost_index = SOURCE_COST_INDEX; 
        }
        if (SINK == rr_graph->rr_node[*cur_node_id].type) {
          rr_graph->rr_node[*cur_node_id].cost_index = SINK_COST_INDEX; 
        }
        /* Switch info */
        rr_graph->rr_node[*cur_node_id].driver_switch = delayless_switch; 
        /* TODO: should we set pb_graph_pin here? */
        /* fill fast look-up table */
        load_one_node_to_rr_graph_fast_lookup(rr_graph, *cur_node_id, 
                                              rr_graph->rr_node[*cur_node_id].type, 
                                              rr_graph->rr_node[*cur_node_id].xlow, 
                                              rr_graph->rr_node[*cur_node_id].ylow,
                                              rr_graph->rr_node[*cur_node_id].ptc_num);
        /* Update node counter */
        (*cur_node_id)++;
      }
    }
  }

  return;
}

/************************************************************************
 * Initialize the basic information of routing track rr_nodes
 * coordinators: xlow, ylow, xhigh, yhigh, 
 * features: capacity, track_ids, ptc_num, direction 
 ***********************************************************************/
static 
void load_one_chan_rr_nodes_basic_info(const DeviceCoordinator& chan_coordinator, 
                                       const t_rr_type chan_type,
                                       ChanNodeDetails* chan_details,
                                       const std::vector<t_segment_inf> segment_infs,
                                       const int cost_index_offset,
                                       t_rr_graph* rr_graph,
                                       size_t* cur_node_id) {
  /* Check each node_id(potential ptc_num) in the channel :
   * If this is a starting point, we set a new rr_node with xlow/ylow, ptc_num
   * If this is a ending point, we set xhigh/yhigh and track_ids
   * For other nodes, we set changes in track_ids
   */
  for (size_t itrack = 0; itrack < chan_details->get_chan_width(); ++itrack) {
    /* For INC direction, a starting point requires a new chan rr_node  */
    if ( (true == chan_details->is_track_start(itrack))
        && (INC_DIRECTION == chan_details->get_track_direction(itrack)) ) {
      /* Use a new chan rr_node  */
      rr_graph->rr_node[*cur_node_id].type  = chan_type; 
      rr_graph->rr_node[*cur_node_id].xlow  = chan_coordinator.get_x(); 
      rr_graph->rr_node[*cur_node_id].ylow  = chan_coordinator.get_y(); 
      rr_graph->rr_node[*cur_node_id].direction = chan_details->get_track_direction(itrack); 
      rr_graph->rr_node[*cur_node_id].ptc_num  = itrack; 
      rr_graph->rr_node[*cur_node_id].track_ids.push_back(itrack); 
      rr_graph->rr_node[*cur_node_id].capacity = 1; 
      rr_graph->rr_node[*cur_node_id].occ = 0; 
      /* assign switch id */
      size_t seg_id = chan_details->get_track_segment_id(itrack);
      rr_graph->rr_node[*cur_node_id].driver_switch = segment_infs[seg_id].opin_switch; 
      /* Update chan_details with node_id */
      chan_details->set_track_node_id(itrack, *cur_node_id);
      /* cost index depends on the segment index */
      rr_graph->rr_node[*cur_node_id].cost_index = cost_index_offset + seg_id; 
      /* Update node counter */
      (*cur_node_id)++;
      /* Finish here, go to next */
    }
    /* For DEC direction, an ending point requires a new chan rr_node  */
    if ( (true == chan_details->is_track_end(itrack))
      && (DEC_DIRECTION == chan_details->get_track_direction(itrack)) ) {
      /* Use a new chan rr_node  */
      rr_graph->rr_node[*cur_node_id].type  = chan_type; 
      rr_graph->rr_node[*cur_node_id].xhigh  = chan_coordinator.get_x(); 
      rr_graph->rr_node[*cur_node_id].yhigh  = chan_coordinator.get_y(); 
      rr_graph->rr_node[*cur_node_id].direction = chan_details->get_track_direction(itrack); 
      rr_graph->rr_node[*cur_node_id].ptc_num  = itrack; 
      rr_graph->rr_node[*cur_node_id].track_ids.push_back(itrack); 
      rr_graph->rr_node[*cur_node_id].capacity = 1; 
      rr_graph->rr_node[*cur_node_id].occ = 0; 
      /* Update chan_details with node_id */
      chan_details->set_track_node_id(itrack, *cur_node_id);
      /* assign switch id */
      size_t seg_id = chan_details->get_track_segment_id(itrack);
      rr_graph->rr_node[*cur_node_id].driver_switch = segment_infs[seg_id].opin_switch; 
      /* cost index depends on the segment index */
      rr_graph->rr_node[*cur_node_id].cost_index = cost_index_offset + seg_id; 
      /* Update node counter */
      (*cur_node_id)++;
      /* Finish here, go to next */
    }
    /* For INC direction, an ending point requires an update on xhigh and yhigh  */
    if ( (true == chan_details->is_track_end(itrack))
      && (INC_DIRECTION == chan_details->get_track_direction(itrack)) ) {
      /* Get the node_id */
      size_t rr_node_id = chan_details->get_track_node_id(itrack);
      /* Do a quick check, make sure we do not mistakenly modify other nodes */
      assert(chan_type == rr_graph->rr_node[rr_node_id].type);
      assert(chan_details->get_track_direction(itrack) == rr_graph->rr_node[rr_node_id].direction);
      /* set xhigh/yhigh and push changes to track_ids */
      rr_graph->rr_node[rr_node_id].xhigh = chan_coordinator.get_x();
      rr_graph->rr_node[rr_node_id].yhigh = chan_coordinator.get_y();
      rr_graph->rr_node[rr_node_id].track_ids.push_back(itrack); 
      /* Finish here, go to next */
    }
    /* For DEC direction, an starting point requires an update on xlow and ylow  */
    if ( (true == chan_details->is_track_start(itrack))
      && (DEC_DIRECTION == chan_details->get_track_direction(itrack)) ) {
      /* Get the node_id */
      size_t rr_node_id = chan_details->get_track_node_id(itrack);
      /* Do a quick check, make sure we do not mistakenly modify other nodes */
      assert(chan_type == rr_graph->rr_node[rr_node_id].type);
      assert(chan_details->get_track_direction(itrack) == rr_graph->rr_node[rr_node_id].direction);
      /* set xhigh/yhigh and push changes to track_ids */
      rr_graph->rr_node[rr_node_id].xlow = chan_coordinator.get_x();
      rr_graph->rr_node[rr_node_id].ylow = chan_coordinator.get_y();
      rr_graph->rr_node[rr_node_id].track_ids.push_back(itrack); 
      /* Finish here, go to next */
    }
    /* For other nodes, we get the node_id and just update track_ids */
    if ( (false == chan_details->is_track_start(itrack))
      && (false == chan_details->is_track_end(itrack)) ) {
      /* Get the node_id */
      size_t rr_node_id = chan_details->get_track_node_id(itrack);
      /* Do a quick check, make sure we do not mistakenly modify other nodes */
      assert(chan_type == rr_graph->rr_node[rr_node_id].type);
      assert(chan_details->get_track_direction(itrack) == rr_graph->rr_node[rr_node_id].direction);
      rr_graph->rr_node[rr_node_id].track_ids.push_back(itrack); 
      /* Finish here, go to next */
    }
    /* fill fast look-up table */
    /* Get node_id */
    int track_node_id = chan_details->get_track_node_id(itrack);
    /* CHANX requires a reverted (x,y) in the fast look-up table */
    if (CHANX == chan_type) {
      load_one_node_to_rr_graph_fast_lookup(rr_graph, track_node_id, 
                                            chan_type, 
                                            chan_coordinator.get_y(),
                                            chan_coordinator.get_x(), 
                                            itrack);
    }
    /* CHANY follows a regular (x,y) in the fast look-up table */
    if (CHANY == chan_type) {
      load_one_node_to_rr_graph_fast_lookup(rr_graph, track_node_id, 
                                            chan_type, 
                                            chan_coordinator.get_x(), 
                                            chan_coordinator.get_y(),
                                            itrack);
    }
  }

  return;
} 

/************************************************************************
 * Initialize the basic information of rr_nodes:
 * coordinators: xlow, ylow, xhigh, yhigh, 
 * features: capacity, track_ids, ptc_num, direction 
 * grid_info : pb_graph_pin
 ***********************************************************************/
static 
void load_rr_nodes_basic_info(t_rr_graph* rr_graph, 
                              const DeviceCoordinator& device_size,
                              const std::vector<std::vector<t_grid_tile>> grids,
                              const std::vector<size_t> chan_width,
                              const std::vector<t_segment_inf> segment_infs,
                              const int wire_to_ipin_switch, const int delayless_switch) {
  /* counter */
  size_t cur_node_id = 0;
  /* configure by node type */ 
  /* SOURCE, SINK, OPIN and IPIN */
  /************************************************************************
   * Search the grid and find the number OPINs and IPINs per grid
   * Note that the number of SOURCE nodes are the same as OPINs
   * and the number of SINK nodes are the same as IPINs
   ***********************************************************************/
  for (size_t ix = 0; ix < device_size.get_x(); ++ix) {
    for (size_t iy = 0; iy < device_size.get_y(); ++iy) { 
      /* Skip EMPTY tiles */
      if (EMPTY_TYPE == grids[ix][iy].type) {
        continue;
      }
      DeviceCoordinator grid_coordinator(ix, iy);
      enum e_side io_side = NUM_SIDES;
      /* If this is the block on borders, we consider IO side */
      if (IO_TYPE == grid[ix][iy].type) {
        DeviceCoordinator io_device_size(device_size.get_x() - 1, device_size.get_y() - 1);
        io_side = determine_io_grid_pin_side(io_device_size, grid_coordinator);
      }
      /* Configure rr_nodes for this grid */
      load_one_grid_rr_nodes_basic_info(grid_coordinator, grid[ix][iy], io_side, 
                                        rr_graph, &cur_node_id, 
                                        wire_to_ipin_switch, delayless_switch);
    }
  }

  /* For X-direction Channel: CHANX */
  for (size_t iy = 0; iy < device_size.get_y() - 1; ++iy) { 
    for (size_t ix = 1; ix < device_size.get_x() - 1; ++ix) {
      DeviceCoordinator chan_coordinator(ix, iy);
      enum e_side chan_side = NUM_SIDES;
      /* For LEFT side of FPGA */
      if (0 == ix) {
        chan_side = LEFT;
      }
      /* For RIGHT side of FPGA */
      if (device_size.get_x() - 2 == ix) {
        chan_side = RIGHT;
      }
      ChanNodeDetails chanx_details = build_unidir_chan_node_details(chan_width[0], device_size.get_x() - 2, chan_side, segment_infs); 
      /* Configure CHANX in this channel */
      load_one_chan_rr_nodes_basic_info(chan_coordinator, CHANX, 
                                        &chanx_details, 
                                        segment_infs, 
                                        CHANX_COST_INDEX_START, 
                                        rr_graph, &cur_node_id);
      /* Rotate the chanx_details by an offset of 1*/
      /* For INC_DIRECTION, we use clockwise rotation 
       * node_id A ---->   -----> node_id D
       * node_id B ---->  / ----> node_id A
       * node_id C ----> /  ----> node_id B
       * node_id D ---->    ----> node_id C 
       */
      chanx_details.rotate_track_node_id(1, INC_DIRECTION, false);
      /* For DEC_DIRECTION, we use clockwise rotation 
       * node_id A <-----    <----- node_id B
       * node_id B <----- \  <----- node_id C
       * node_id C <-----  \ <----- node_id D
       * node_id D <-----    <----- node_id A 
       */
      chanx_details.rotate_track_node_id(1, DEC_DIRECTION, true);
    }
  }

  /* For Y-direction Channel: CHANX */
  for (size_t ix = 0; ix < device_size.get_x() - 1; ++ix) {
    for (size_t iy = 1; iy < device_size.get_y() - 1; ++iy) { 
      DeviceCoordinator chan_coordinator(ix, iy);
      enum e_side chan_side = NUM_SIDES;
      /* For LEFT side of FPGA */
      if (0 == iy) {
        chan_side = BOTTOM;
      }
      /* For RIGHT side of FPGA */
      if (device_size.get_y() - 2 == iy) {
        chan_side = TOP;
      }
      ChanNodeDetails chany_details = build_unidir_chan_node_details(chan_width[1], device_size.get_y() - 2, chan_side, segment_infs); 
      /* Configure CHANX in this channel */
      load_one_chan_rr_nodes_basic_info(chan_coordinator, CHANY, 
                                        &chany_details, 
                                        segment_infs, 
                                        CHANX_COST_INDEX_START + segment_infs.size(), 
                                        rr_graph, &cur_node_id);
      /* Rotate the chany_details by an offset of 1*/
      /* For INC_DIRECTION, we use clockwise rotation 
       * node_id A ---->   -----> node_id D
       * node_id B ---->  / ----> node_id A
       * node_id C ----> /  ----> node_id B
       * node_id D ---->    ----> node_id C 
       */
      chany_details.rotate_track_node_id(1, INC_DIRECTION, false);
      /* For DEC_DIRECTION, we use clockwise rotation 
       * node_id A <-----    <----- node_id B
       * node_id B <----- \  <----- node_id C
       * node_id C <-----  \ <----- node_id D
       * node_id D <-----    <----- node_id A 
       */
      chany_details.rotate_track_node_id(1, DEC_DIRECTION, true);
    }
  }

  /* Check */
  assert ((int)cur_node_id == rr_graph->num_rr_nodes);

  /* Reverse the track_ids of CHANX and CHANY nodes in DEC_DIRECTION*/
  for (int inode = 0; inode < rr_graph->num_rr_nodes; ++inode) {
    /* Bypass condition: only focus on CHANX and CHANY in DEC_DIRECTION */
    if ( (CHANX != rr_graph->rr_node[inode].type)
      && (CHANY != rr_graph->rr_node[inode].type) ) {
      continue;
    }
    /* Reach here, we must have a node of CHANX or CHANY */
    if (DEC_DIRECTION != rr_graph->rr_node[inode].direction) {
      continue;
    }
    std::reverse(rr_graph->rr_node[inode].track_ids.begin(),
                 rr_graph->rr_node[inode].track_ids.end() );
  }

  return;
}

/************************************************************************
 * Build a fast look-up for the rr_nodes
 * it is a 4-dimension array to categorize rr_nodes in terms of 
 * types, coordinators and ptc_num (feature number)
 * The results will be stored in rr_node_indices[type][x][y]
 ***********************************************************************/
static 
void alloc_rr_graph_fast_lookup(const DeviceCoordinator& device_size,
                                t_rr_graph* rr_graph) {
  /* Allocates and loads all the structures needed for fast lookups of the   *
   * index of an rr_node.  rr_node_indices is a matrix containing the index  *
   * of the *first* rr_node at a given (i,j) location.                       */

  /* Alloc the lookup table */
  rr_graph->rr_node_indices = (t_ivec ***) my_malloc(sizeof(t_ivec **) * NUM_RR_TYPES);

  /* For OPINs, IPINs, SOURCE, SINKs, CHANX and CHANY */
  for (int type = 0; type < NUM_RR_TYPES; ++type) {
    /* Skip SOURCE and OPIN, they will share with SOURCE and SINK
   * SOURCE and SINK have unique ptc values so their data can be shared.
   * IPIN and OPIN have unique ptc values so their data can be shared. 
   */
    if ((SOURCE == type) || (OPIN == type) ) {
      continue;
    }
    rr_graph->rr_node_indices[type] = (t_ivec **) my_malloc(sizeof(t_ivec *) * device_size.get_x());
    for (size_t i = 0; i < device_size.get_x(); ++i) {
      rr_graph->rr_node_indices[type][i] = (t_ivec *) my_malloc(sizeof(t_ivec) * device_size.get_y());
      for (size_t j = 0; j < device_size.get_y(); ++j) {
        rr_graph->rr_node_indices[type][i][j].nelem = 0;
        rr_graph->rr_node_indices[type][i][j].list = NULL;
      }
    }
  }

  /* SOURCE and SINK have unique ptc values so their data can be shared.
   * IPIN and OPIN have unique ptc values so their data can be shared. */
  rr_graph->rr_node_indices[SOURCE] = rr_graph->rr_node_indices[SINK];
  rr_graph->rr_node_indices[OPIN] = rr_graph->rr_node_indices[IPIN];

  return;
}

/************************************************************************
 * Build the edges of each rr_node tile by tile:
 * We classify rr_nodes into a general switch block (GSB) data structure
 * where we create edges to each rr_nodes in the GSB with respect to
 * Fc_in and Fc_out, switch block patterns 
 * For each GSB: 
 * 1. create edges between SOURCE and OPINs
 * 2. create edges between IPINs and SINKs
 * 3. create edges between CHANX | CHANY and IPINs (connections inside connection blocks)
 * 4. create edges between OPINs, CHANX and CHANY (connections inside switch blocks)
 * 5. create edges between OPINs and IPINs (direct-connections)
 ***********************************************************************/
static 
void build_rr_graph_edges(t_rr_graph* rr_graph, 
                          const DeviceCoordinator& device_size, 
                          const std::vector< std::vector<t_grid_tile> > grids,
                          const std::vector<size_t> device_chan_width, 
                          const std::vector<t_segment_inf> segment_inf,
                          int** Fc_in, int** Fc_out,
                          const enum e_switch_block_type sb_type, const int Fs) {

  DeviceCoordinator device_range(device_size.get_x() - 1, device_size.get_y() - 1);

  /* Go Switch Block by Switch Block */
  for (size_t ix = 0; ix < device_size.get_x(); ++ix) {
    for (size_t iy = 0; iy < device_size.get_y(); ++iy) { 

      DeviceCoordinator gsb_coordinator(ix, iy);
      /* Create a GSB object */
      RRGSB rr_gsb = build_one_tileable_rr_gsb(device_range, device_chan_width, segment_inf, gsb_coordinator, rr_graph);

      DeviceCoordinator grid_coordinator = rr_gsb.get_grid_coordinator();

      /* adapt the track_to_ipin_lookup for the GSB nodes */      
      t_track2pin_map track2ipin_map; /* [0..track_gsb_side][0..num_tracks][ipin_indices] */
      /* Get the Fc index of the grid */
      int grid_Fc_in_index = grids[grid_coordinator.get_x()][grid_coordinator.get_x()].type->index;
      track2ipin_map = build_gsb_track_to_ipin_map(rr_graph, rr_gsb, segment_inf, Fc_in[grid_Fc_in_index]);

      /* adapt the opin_to_track_map for the GSB nodes */      
      t_pin2track_map opin2track_map; /* [0..gsb_side][0..num_opin_node][track_indices] */
      /* Get the Fc index of the grid */
      int grid_Fc_out_index = grids[grid_coordinator.get_x()][grid_coordinator.get_x()].type->index;
      opin2track_map = build_gsb_opin_to_track_map(rr_graph, rr_gsb, segment_inf, Fc_out[grid_Fc_out_index]);

      /* adapt the switch_block_conn for the GSB nodes */      
      t_track2track_map sb_conn; /* [0..from_gsb_side][0..chan_width-1][track_indices] */
      sb_conn = build_gsb_track_to_track_map(rr_graph, rr_gsb, sb_type, Fs, segment_inf);

      /* Build edges for a GSB */
      build_edges_for_one_tileable_rr_gsb(rr_graph, &rr_gsb, 
                                          track2ipin_map, opin2track_map, 
                                          sb_conn);
      /* Finish this GSB, go to the next*/
    }
  }

  return;
}

/************************************************************************
 * Build direct edges for Grids *
 ***********************************************************************/
static 
void build_rr_graph_direct_connections(t_rr_graph* rr_graph, 
                                       const DeviceCoordinator& device_size,
                                       const std::vector< std::vector<t_grid_tile> > grids, 
                                       const int delayless_switch, 
                                       const int num_directs, 
                                       const t_direct_inf *directs, 
                                       const t_clb_to_clb_directs *clb_to_clb_directs) {
  for (size_t ix = 0; ix < device_size.get_x(); ++ix) {
    for (size_t iy = 0; iy < device_size.get_y(); ++iy) { 
      /* Skip EMPTY tiles */
      if (EMPTY_TYPE == grids[ix][iy].type) {
        continue;
      }
      /* Skip height>1 tiles (mostly heterogeneous blocks) */
      if (0 < grids[ix][iy].offset) {
        continue;
      }
      DeviceCoordinator from_grid_coordinator(ix, iy);
      build_direct_connections_for_one_gsb(rr_graph, device_size, grids,
                                           from_grid_coordinator,
                                           grids[ix][iy],
                                           delayless_switch, 
                                           num_directs, directs, clb_to_clb_directs);
    }
  }

  return;
}

/************************************************************************
 * Main function of this file
 * Builder for a detailed uni-directional tileable rr_graph
 * Global graph is not supported here, the VPR rr_graph generator can be used  
 * It follows the procedures to complete the rr_graph generation
 * 1. Assign the segments for each routing channel,
 *    To be specific, for each routing track, we assign a routing segment.
 *    The assignment is subject to users' specifications, such as 
 *    a. length of each type of segment
 *    b. frequency of each type of segment.
 *    c. routing channel width
 * 2. Estimate the number of nodes in the rr_graph
 *    This will estimate the number of 
 *    a. IPINs, input pins of each grid
 *    b. OPINs, output pins of each grid
 *    c. SOURCE, virtual node which drives OPINs
 *    d. SINK, virtual node which is connected to IPINs
 *    e. CHANX and CHANY, routing segments of each channel
 * 3. Create the connectivity of OPINs
 *    a. Evenly assign connections to OPINs to routing tracks
 *    b. the connection pattern should be same across the fabric
 * 4. Create the connectivity of IPINs 
 *    a. Evenly assign connections from routing tracks to IPINs
 *    b. the connection pattern should be same across the fabric
 * 5. Create the switch block patterns, 
 *    It is based on the type of switch block, the supported patterns are 
 *    a. Disjoint, which connects routing track (i)th from (i)th and (i)th routing segments
 *    b. Universal, which connects routing track (i)th from (i)th and (M-i)th routing segments
 *    c. Wilton, which rotates the connection of Disjoint by 1 track
 * 6. Allocate rr_graph, fill the node information
 *    For each node, fill
 *    a. basic information: coordinator(xlow, xhigh, ylow, yhigh), ptc_num
 *    b. edges (both incoming and outcoming)
 *    c. handle direct-connections
 * 7. Build fast look-up for the rr_graph 
 * 8. Allocate external data structures
 *    a. cost_index
 *    b. RC tree
 ***********************************************************************/
void build_tileable_unidir_rr_graph(INP const int L_num_types,
                                    INP t_type_ptr types, INP const int L_nx, INP const int L_ny,
                                    INP struct s_grid_tile **L_grid, INP const int chan_width,
                                    INP const enum e_switch_block_type sb_type, INP const int Fs, 
                                    INP const int num_seg_types,
                                    INP const t_segment_inf * segment_inf,
                                    INP const int num_switches, INP const int delayless_switch, 
                                    INP const int global_route_switch,
                                    INP const t_timing_inf timing_inf, 
                                    INP const int wire_to_ipin_switch,
                                    INP const enum e_base_cost_type base_cost_type, 
                                    INP const t_direct_inf *directs, 
                                    INP const int num_directs, INP const boolean ignore_Fc_0, 
                                    OUTP int *Warnings) { 
  /* Create an empty graph */
  t_rr_graph rr_graph; 
  rr_graph.rr_node_indices = NULL;
  rr_graph.rr_node = NULL;
  rr_graph.num_rr_nodes = 0;

  /* Reset warning flag */
  *Warnings = RR_GRAPH_NO_WARN;

  /* Print useful information on screen */
  vpr_printf(TIO_MESSAGE_INFO, 
             "Creating tileable Routing Resource(RR) graph...\n");

  /* Create a matrix of grid */
  DeviceCoordinator device_size(L_nx + 2, L_ny + 2);
  std::vector< std::vector<t_grid_tile> > grids;
  /* reserve vector capacity to be memory efficient */
  grids.resize(L_nx + 2);
  for (int ix = 0; ix < (L_nx + 2); ++ix) {
    grids[ix].resize(L_ny + 2);
    for (int iy = 0; iy < (L_ny + 2); ++iy) {
      grids[ix][iy] = L_grid[ix][iy];
    }
  }
  /* Create a vector of channel width, we support X-direction and Y-direction has different W */
  std::vector<size_t> device_chan_width;
  device_chan_width.push_back(chan_width);
  device_chan_width.push_back(chan_width);

  /* Create a vector of segment_inf */
  std::vector<t_segment_inf> segment_infs;
  for (int iseg = 0; iseg < num_seg_types; ++iseg) {
    segment_infs.push_back(segment_inf[iseg]);
  }

  /************************************************************************
   * 2. Estimate the number of nodes in the rr_graph
   *    This will estimate the number of 
   *    a. IPINs, input pins of each grid
   *    b. OPINs, output pins of each grid
   *    c. SOURCE, virtual node which drives OPINs
   *    d. SINK, virtual node which is connected to IPINs
   *    e. CHANX and CHANY, routing segments of each channel
   ***********************************************************************/
  std::vector<size_t> num_rr_nodes_per_type = estimate_num_rr_nodes_per_type(device_size, grids, device_chan_width, segment_infs); 

  /************************************************************************
   * 3. Allocate the rr_nodes 
   ***********************************************************************/
  rr_graph.num_rr_nodes = 0;
  for (size_t i = 0; i < num_rr_nodes_per_type.size(); ++i) {
    rr_graph.num_rr_nodes += num_rr_nodes_per_type[i];
  }
  /* use calloc and memset to initialize everything to be zero */
  rr_graph.rr_node = (t_rr_node*)my_calloc(rr_graph.num_rr_nodes, sizeof(t_rr_node));
  for (int i = 0; i < rr_graph.num_rr_nodes; ++i) {
    tileable_rr_graph_init_rr_node(&(rr_graph.rr_node[i]));
  }

  vpr_printf(TIO_MESSAGE_INFO, 
             "%d RR graph nodes allocated.\n", rr_graph.num_rr_nodes);

  /************************************************************************
   * 4. Initialize the basic information of rr_nodes:
   *    coordinators: xlow, ylow, xhigh, yhigh, 
   *    features: capacity, track_ids, ptc_num, direction 
   *    grid_info : pb_graph_pin
   ***********************************************************************/
  alloc_rr_graph_fast_lookup(device_size, &rr_graph);

  load_rr_nodes_basic_info(&rr_graph, device_size, grids, device_chan_width, segment_infs,
                           wire_to_ipin_switch, delayless_switch); 

  vpr_printf(TIO_MESSAGE_INFO, 
             "Built node basic information and fast-look.\n");

  /************************************************************************
   * 5.1 Create the connectivity of OPINs
   *     a. Evenly assign connections to OPINs to routing tracks
   *     b. the connection pattern should be same across the fabric
   *
   * 5.2 Create the connectivity of IPINs 
   *     a. Evenly assign connections from routing tracks to IPINs
   *     b. the connection pattern should be same across the fabric
   ***********************************************************************/
  int **Fc_in = NULL; /* [0..num_types-1][0..num_pins-1] */
  boolean Fc_clipped;
  Fc_clipped = FALSE;
  Fc_in = alloc_and_load_actual_fc(L_num_types, types, chan_width,
                                   FALSE, UNI_DIRECTIONAL, &Fc_clipped, ignore_Fc_0);
  if (Fc_clipped) {
    *Warnings |= RR_GRAPH_WARN_FC_CLIPPED;
  }

  int **Fc_out = NULL; /* [0..num_types-1][0..num_pins-1] */
  Fc_clipped = FALSE;
  Fc_out = alloc_and_load_actual_fc(L_num_types, types, chan_width,
                                   TRUE, UNI_DIRECTIONAL, &Fc_clipped, ignore_Fc_0);

  if (Fc_clipped) {
    *Warnings |= RR_GRAPH_WARN_FC_CLIPPED;
  }

  vpr_printf(TIO_MESSAGE_INFO, 
             "Actual Fc numbers loaded.\n");

  /************************************************************************
   * 6. Build the connections tile by tile:
   *    We classify rr_nodes into a general switch block (GSB) data structure
   *    where we create edges to each rr_nodes in the GSB with respect to
   *    Fc_in and Fc_out, switch block patterns 
   *    In addition, we will also handle direct-connections:
   *    Add edges that bridge OPINs and IPINs to the rr_graph
   ***********************************************************************/

  /* Create edges for a tileable rr_graph */
  build_rr_graph_edges(&rr_graph, device_size, grids, device_chan_width, segment_infs, 
                       Fc_in, Fc_out,
                       sb_type, Fs);

  vpr_printf(TIO_MESSAGE_INFO, 
             "Regular edges of RR graph built.\n");

  /************************************************************************
   * 7. Build direction connection lists
   ***********************************************************************/
  /* Create data structure of direct-connections */
  t_clb_to_clb_directs* clb_to_clb_directs = NULL;
  if (num_directs > 0) {
    clb_to_clb_directs = alloc_and_load_clb_to_clb_directs(directs, num_directs);
  }
  build_rr_graph_direct_connections(&rr_graph, device_size, grids, delayless_switch, 
                                    num_directs, directs, clb_to_clb_directs);

  vpr_printf(TIO_MESSAGE_INFO, 
             "Direct-connection edges of RR graph built.\n");

  /************************************************************************
   * 8. Allocate external data structures
   *    a. cost_index
   *    b. RC tree
   ***********************************************************************/
  /* We set global variables for rr_nodes here, they will be updated by rr_graph_external */
  num_rr_nodes = rr_graph.num_rr_nodes;
  rr_node = rr_graph.rr_node;
  rr_node_indices = rr_graph.rr_node_indices;

  rr_graph_externals(timing_inf, segment_inf, num_seg_types, chan_width,
                     wire_to_ipin_switch, base_cost_type);

  /************************************************************************
   * 9. Sanitizer for the rr_graph, check connectivities of rr_nodes
   ***********************************************************************/

  /* Print useful information on screen */
  vpr_printf(TIO_MESSAGE_INFO, 
             "Create a tileable RR graph with %d nodes\n", 
             num_rr_nodes);

  check_rr_graph(GRAPH_UNIDIR_TILEABLE, types, L_nx, L_ny, chan_width, Fs,
                 num_seg_types, num_switches, segment_inf, global_route_switch,
                 delayless_switch, wire_to_ipin_switch, Fc_in, Fc_out);

  /* Print useful information on screen */
  vpr_printf(TIO_MESSAGE_INFO, 
             "Tileable Routing Resource(RR) graph pass checking.\n");


  /************************************************************************
   * 10. Free all temp stucts 
   ***********************************************************************/

  /* Free all temp structs */
  if (Fc_in) {
    free_matrix(Fc_in,0, L_num_types, 0, sizeof(int));
    Fc_in = NULL;
  }
  if (Fc_out) {
    free_matrix(Fc_out,0, L_num_types, 0, sizeof(int));
    Fc_out = NULL;
  }
  if(clb_to_clb_directs != NULL) {
    free(clb_to_clb_directs);
  }

  return;
}

/************************************************************************
 * End of file : rr_graph_tileable_builder.c 
 ***********************************************************************/
