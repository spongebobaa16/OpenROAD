// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "RepairHold.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "RepairDesign.hh"
#include "db_sta/dbNetwork.hh"
#include "rsz/Resizer.hh"
#include "sta/Corner.hh"
#include "sta/DcalcAnalysisPt.hh"
#include "sta/Fuzzy.hh"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/InputDrive.hh"
#include "sta/Liberty.hh"
#include "sta/Parasitics.hh"
#include "sta/PathExpanded.hh"
#include "sta/PortDirection.hh"
#include "sta/Sdc.hh"
#include "sta/Search.hh"
#include "sta/TimingArc.hh"
#include "sta/Units.hh"
#include "utl/Logger.h"

namespace rsz {

using std::max;
using std::min;
using std::string;
using std::vector;

using utl::RSZ;

using sta::Edge;
using sta::fuzzyLess;
using sta::INF;
using sta::PathExpanded;
using sta::Port;
using sta::VertexOutEdgeIterator;

RepairHold::RepairHold(Resizer* resizer) : resizer_(resizer)
{
}

void RepairHold::init()
{
  logger_ = resizer_->logger_;
  dbStaState::init(resizer_->sta_);
  db_network_ = resizer_->db_network_;
  initial_design_area_ = resizer_->computeDesignArea();
  hold_buffer_insertion_passes = 0;  // Initialize SA counter
}

bool RepairHold::repairHold(
    const double setup_margin,
    const double hold_margin,
    const bool allow_setup_violations,
    // Max buffer count as percent of design instance count.
    const float max_buffer_percent,
    const int max_passes,
    const bool verbose)
{
  bool repaired = false;
  init();
  sta_->checkSlewLimitPreamble();
  sta_->checkCapacitanceLimitPreamble();
  LibertyCell* buffer_cell = findHoldBuffer();

  sta_->findRequireds();
  VertexSet* ends = sta_->search()->endpoints();
  VertexSeq ends1;
  for (Vertex* end : *ends) {
    ends1.push_back(end);
  }
  sort(ends1, sta::VertexIdLess(graph_));

  int max_buffer_count = max_buffer_percent * network_->instanceCount();
  // Prevent it from being too small on trivial designs
  max_buffer_count = std::max(max_buffer_count, 100);

  {
    IncrementalParasiticsGuard guard(resizer_);
    repaired = repairHold(ends1,
                          buffer_cell,
                          setup_margin,
                          hold_margin,
                          allow_setup_violations,
                          max_buffer_count,
                          max_passes,
                          verbose);
  }

  return repaired;
}

// For testing/debug.
void RepairHold::repairHold(const Pin* end_pin,
                            const double setup_margin,
                            const double hold_margin,
                            const bool allow_setup_violations,
                            const float max_buffer_percent,
                            const int max_passes)
{
  init();
  sta_->checkSlewLimitPreamble();
  sta_->checkCapacitanceLimitPreamble();
  LibertyCell* buffer_cell = findHoldBuffer();

  Vertex* end = graph_->pinLoadVertex(end_pin);
  VertexSeq ends;
  ends.push_back(end);

  sta_->findRequireds();
  const int max_buffer_count = max_buffer_percent * network_->instanceCount();

  {
    IncrementalParasiticsGuard guard(resizer_);
    repairHold(ends,
               buffer_cell,
               setup_margin,
               hold_margin,
               allow_setup_violations,
               max_buffer_count,
               max_passes,
               false);
  }
}

// Find a good hold buffer using delay/area as the metric.
LibertyCell* RepairHold::findHoldBuffer()
{
  // Build a vector of buffers sorted by the metric
  struct MetricBuffer
  {
    float metric;
    LibertyCell* cell;
  };
  std::vector<MetricBuffer> buffers;
  for (LibertyCell* buffer : resizer_->buffer_cells_) {
    const float buffer_area = buffer->area();
    if (buffer_area != 0.0) {
      const float buffer_cost = bufferHoldDelay(buffer) / buffer_area;
      buffers.push_back({buffer_cost, buffer});
    }
  }

  std::sort(buffers.begin(),
            buffers.end(),
            [](const MetricBuffer& lhs, const MetricBuffer& rhs) {
              return lhs.metric < rhs.metric;
            });

  if (buffers.empty()) {
    return nullptr;
  }

  // Select the highest metric
  MetricBuffer& best_buffer = *buffers.rbegin();
  const MetricBuffer& highest_metric = best_buffer;

  // See if there is a smaller choice with nearly as good a metric.
  const float margin = 0.95;
  for (auto itr = buffers.rbegin() + 1; itr != buffers.rend(); itr++) {
    if (itr->metric >= margin * highest_metric.metric) {
      // buffer within margin, so check if area is smaller
      const float best_buffer_area = best_buffer.cell->area();
      const float buffer_area = itr->cell->area();

      if (buffer_area < best_buffer_area) {
        best_buffer = *itr;
      }
    }
  }

  return best_buffer.cell;
}

float RepairHold::bufferHoldDelay(LibertyCell* buffer)
{
  Delay delays[RiseFall::index_count];
  bufferHoldDelays(buffer, delays);
  return min(delays[RiseFall::riseIndex()], delays[RiseFall::fallIndex()]);
}

// Min self delay across corners; buffer -> buffer
void RepairHold::bufferHoldDelays(LibertyCell* buffer,
                                  // Return values.
                                  Delay delays[RiseFall::index_count])
{
  LibertyPort *input, *output;
  buffer->bufferPorts(input, output);

  for (int rf_index : RiseFall::rangeIndex()) {
    delays[rf_index] = MinMax::min()->initValue();
  }
  for (Corner* corner : *sta_->corners()) {
    LibertyPort* corner_port = input->cornerPort(corner->libertyIndex(max_));
    const DcalcAnalysisPt* dcalc_ap = corner->findDcalcAnalysisPt(max_);
    const float load_cap = corner_port->capacitance();
    ArcDelay gate_delays[RiseFall::index_count];
    Slew slews[RiseFall::index_count];
    resizer_->gateDelays(output, load_cap, dcalc_ap, gate_delays, slews);
    for (int rf_index : RiseFall::rangeIndex()) {
      delays[rf_index] = min(delays[rf_index], gate_delays[rf_index]);
    }
  }
}

bool RepairHold::repairHold(VertexSeq& ends,
                            LibertyCell* buffer_cell,
                            const double setup_margin,
                            const double hold_margin,
                            const bool allow_setup_violations,
                            const int max_buffer_count,
                            const int max_passes,
                            const bool verbose)
{
  bool repaired = false;
  // Find endpoints with hold violations.
  VertexSeq hold_failures;
  Slack worst_slack;
  findHoldViolations(ends, hold_margin, worst_slack, hold_failures);
  inserted_buffer_count_ = 0;
  if (!hold_failures.empty()) {
    logger_->info(RSZ,
                  46,
                  "Found {} endpoints with hold violations.",
                  hold_failures.size());
    bool progress = true;
    printProgress(0, true, false);
    int pass = 1;
    while (worst_slack < hold_margin && progress && !resizer_->overMaxArea()
           && inserted_buffer_count_ <= max_buffer_count
           && pass <= max_passes) {
      if (verbose || pass == 1) {
        printProgress(pass, false, false);
      }
      debugPrint(logger_,
                 RSZ,
                 "repair_hold",
                 1,
                 "pass {} hold slack {} setup slack {}",
                 pass,
                 delayAsString(worst_slack, sta_, 3),
                 delayAsString(sta_->worstSlack(max_), sta_, 3));
      int hold_buffer_count_before = inserted_buffer_count_;
      repairHoldPass(hold_failures,
                     buffer_cell,
                     setup_margin,
                     hold_margin,
                     allow_setup_violations,
                     max_buffer_count,
                     verbose,
                     pass);
      debugPrint(logger_,
                 RSZ,
                 "repair_hold",
                 1,
                 "inserted {}",
                 inserted_buffer_count_ - hold_buffer_count_before);
      sta_->findRequireds();
      findHoldViolations(ends, hold_margin, worst_slack, hold_failures);
      progress = inserted_buffer_count_ > hold_buffer_count_before;
    }
    printProgress(pass, true, true);
    if (hold_margin == 0.0 && fuzzyLess(worst_slack, 0.0)) {
      logger_->warn(RSZ, 66, "Unable to repair all hold violations.");
    } else if (fuzzyLess(worst_slack, hold_margin)) {
      logger_->warn(RSZ, 64, "Unable to repair all hold checks within margin.");
    }

    if (inserted_buffer_count_ > 0) {
      repaired = true;
      logger_->info(
          RSZ, 32, "Inserted {} hold buffers.", inserted_buffer_count_);
      resizer_->level_drvr_vertices_valid_ = false;
    }
    if (inserted_buffer_count_ > max_buffer_count) {
      logger_->error(RSZ, 60, "Max buffer count reached.");
    }
    if (resizer_->overMaxArea()) {
      logger_->error(RSZ, 50, "Max utilization reached.");
    }
  } else {
    repaired = false;
    logger_->info(RSZ, 33, "No hold violations found.");
  }
  logger_->metric("design__instance__count__hold_buffer",
                  inserted_buffer_count_);

  return repaired;
}

void RepairHold::findHoldViolations(VertexSeq& ends,
                                    const double hold_margin,
                                    // Return values.
                                    Slack& worst_slack,
                                    VertexSeq& hold_violations)
{
  worst_slack = INF;
  hold_violations.clear();
  debugPrint(logger_, RSZ, "repair_hold", 3, "Hold violations");
  for (Vertex* end : ends) {
    const Slack slack = sta_->vertexSlack(end, min_);
    if (!sta_->isClock(end->pin()) && slack < hold_margin) {
      debugPrint(logger_,
                 RSZ,
                 "repair_hold",
                 3,
                 " {} hold_slack={} setup_slack={}",
                 end->name(sdc_network_),
                 delayAsString(slack, sta_),
                 delayAsString(sta_->vertexSlack(end, max_), sta_));
      if (slack < worst_slack) {
        worst_slack = slack;
      }
      hold_violations.push_back(end);
    }
  }
}

void RepairHold::repairHoldPass(VertexSeq& hold_failures,
                                LibertyCell* buffer_cell,
                                const double setup_margin,
                                const double hold_margin,
                                const bool allow_setup_violations,
                                const int max_buffer_count,
                                bool verbose,
                                int& pass)
{
  resizer_->updateParasitics();
  sort(hold_failures, [=](Vertex* end1, Vertex* end2) {
    return sta_->vertexSlack(end1, min_) < sta_->vertexSlack(end2, min_);
  });
  for (Vertex* end_vertex : hold_failures) {
    if (verbose) {
      printProgress(pass, false, false);
    }

    resizer_->updateParasitics();
    repairEndHold(end_vertex,
                  buffer_cell,
                  setup_margin,
                  hold_margin,
                  allow_setup_violations);
    pass++;
    if (inserted_buffer_count_ > max_buffer_count) {
      break;
    }
  }
}

void RepairHold::repairEndHold(Vertex* end_vertex,
                               LibertyCell* buffer_cell,
                               const double setup_margin,
                               const double hold_margin,
                               const bool allow_setup_violations)
{
  Path* end_path = sta_->vertexWorstSlackPath(end_vertex, min_);
  if (end_path) {
    debugPrint(logger_,
               RSZ,
               "repair_hold",
               3,
               "repair end {} hold_slack={} setup_slack={}",
               end_vertex->name(network_),
               delayAsString(end_path->slack(sta_), sta_),
               delayAsString(sta_->vertexSlack(end_vertex, max_), sta_));
    PathExpanded expanded(end_path, sta_);
    sta::SearchPredNonLatch2 pred(sta_);
    const int path_length = expanded.size();
    if (path_length > 1) {
      sta::VertexSeq path_vertices;
      // Inserting bufferes invalidates the paths so copy out the vertices
      // in the path.
      for (int i = expanded.startIndex(); i < path_length; i++) {
        path_vertices.push_back(expanded.path(i)->vertex(sta_));
      }
      // Stop one short of the end so we can get the load.
      for (int i = 0; i < path_vertices.size() - 1; i++) {
        Vertex* path_vertex = path_vertices[i];
        Pin* path_pin = path_vertex->pin();
        // explicitly force getting the flat net.
        odb::dbNet* db_path_net
            = network_->isTopLevelPort(path_pin)
                  ? db_network_->flatNet(network_->term(path_pin))
                  : db_network_->flatNet(const_cast<Pin*>(path_pin));

        if (path_vertex->isDriver(network_) && !resizer_->dontTouch(path_pin)
            && !db_path_net->isConnectedByAbutment()) {
          PinSeq load_pins;
          Slacks slacks;
          mergeInit(slacks);
          float excluded_cap = 0.0;
          bool loads_have_out_port = false;
          VertexOutEdgeIterator edge_iter(path_vertex, graph_);
          while (edge_iter.hasNext()) {
            Edge* edge = edge_iter.next();
            Vertex* fanout = edge->to(graph_);
            if (pred.searchTo(fanout) && pred.searchThru(edge)) {
              Slack fanout_hold_slack = sta_->vertexSlack(fanout, min_);
              Pin* load_pin = fanout->pin();
              if (fanout_hold_slack < hold_margin) {
                load_pins.push_back(load_pin);
                Slacks fanout_slacks;
                sta_->vertexSlacks(fanout, fanout_slacks);
                mergeInto(fanout_slacks, slacks);
                if (network_->direction(load_pin)->isAnyOutput()
                    && network_->isTopLevelPort(load_pin)) {
                  loads_have_out_port = true;
                }
              } else {
                LibertyPort* load_port = network_->libertyPort(load_pin);
                if (load_port) {
                  excluded_cap += load_port->capacitance();
                }
              }
            }
          }
          if (!load_pins.empty()) {
            debugPrint(logger_,
                       RSZ,
                       "repair_hold",
                       3,
                       " {} hold_slack={}/{} setup_slack={}/{} fanouts={}",
                       path_vertex->name(network_),
                       delayAsString(slacks[rise_index_][min_index_], sta_),
                       delayAsString(slacks[fall_index_][min_index_], sta_),
                       delayAsString(slacks[rise_index_][max_index_], sta_),
                       delayAsString(slacks[fall_index_][max_index_], sta_),
                       load_pins.size());
            const DcalcAnalysisPt* dcalc_ap
                = sta_->cmdCorner()->findDcalcAnalysisPt(max_);
            float load_cap
                = graph_delay_calc_->loadCap(end_vertex->pin(), dcalc_ap)
                  - excluded_cap;
            ArcDelay buffer_delays[RiseFall::index_count];
            Slew buffer_slews[RiseFall::index_count];
            resizer_->bufferDelays(
                buffer_cell, load_cap, dcalc_ap, buffer_delays, buffer_slews);
            // setup_slack > -hold_slack
            if (allow_setup_violations
                // || (slacks[rise_index_][max_index_] - setup_margin
                //         > -(slacks[rise_index_][min_index_] - hold_margin)
                //     && slacks[fall_index_][max_index_] - setup_margin
                //            > -(slacks[fall_index_][min_index_] - hold_margin)
                //     // enough slack to insert the buffer
                //     // setup_slack > buffer_delay
                //     && (slacks[rise_index_][max_index_] - setup_margin)
                //            > buffer_delays[rise_index_]
                //     && (slacks[fall_index_][max_index_] - setup_margin)
                //            > buffer_delays[fall_index_])
                ) {
              Vertex* path_load = path_vertices[i + 1];
              Point path_load_loc = db_network_->location(path_load->pin());
              Point drvr_loc = db_network_->location(path_vertex->pin());
              // Point buffer_loc((drvr_loc.x() + path_load_loc.x()) / 2,
              //                  (drvr_loc.y() + path_load_loc.y()) / 2);
              Point buffer_loc(drvr_loc.x()+4*(path_load_loc.x()-drvr_loc.x())/5,
                                  drvr_loc.y()+4*(path_load_loc.y()-drvr_loc.y())/5);
              // Despite checking for setup slack to insert the bufffer,
              // increased slews downstream can increase delays and
              // reduce setup slack in ways that are too expensive to
              // predict. Use the journal to back out the change if
              // the hold buffer blows through the setup margin.
              resizer_->journalBegin();
              Slack setup_slack_before = sta_->worstSlack(max_);
              Slew slew_before = sta_->vertexSlew(path_vertex, max_);
              makeHoldDelay(path_vertex,
                            load_pins,
                            loads_have_out_port,
                            buffer_cell,
                            buffer_loc);
              Slew slew_after = sta_->vertexSlew(path_vertex, max_);
              Slack setup_slack_after = sta_->worstSlack(max_);
              float slew_factor
                  = (slew_before > 0) ? slew_after / slew_before : 1.0;

              // Calculate slack changes for SA evaluation
              float setup_slack_delta = setup_slack_before - setup_slack_after;  // positive = worse setup
              
              // Traditional rejection criteria
              bool traditional_reject = (slew_factor > 1.20
                  || (!allow_setup_violations
                      && fuzzyLess(setup_slack_after, setup_slack_before)
                      && setup_slack_after < setup_margin));
              
              bool accept_change = false;
              
              if (!traditional_reject) {
                // Traditional acceptance case
                accept_change = true;
              } else {
                // Apply SA for traditionally rejected changes
                // Temperature schedule: start high, decrease over iterations
                float initial_temp = 1e-10f;  // Adjusted for typical slack magnitude
                float cooling_rate = 0.95f;
                float min_temp = 1e-14f;
                float temp_calc = initial_temp * pow(cooling_rate, hold_buffer_insertion_passes);
                float current_temp = (temp_calc > min_temp) ? temp_calc : min_temp;
                
                // SA acceptance probability 
                float acceptance_prob = exp(-setup_slack_delta / current_temp);
                
                // Generate random number for acceptance decision
                float random_val = static_cast<float>(rand()) / RAND_MAX;
                bool accept_worse = (random_val < acceptance_prob);
                
                if (accept_worse) {
                  accept_change = true;
                  debugPrint(logger_, RSZ, "repair_hold_SA", 1, 
                           "SA accepted worse buffer insertion: "
                           "slack_delta={:.3e}, temp={:.3e}, prob={:.3f}, rand={:.3f}",
                           setup_slack_delta, current_temp, acceptance_prob, random_val);
                } else {
                  debugPrint(logger_, RSZ, "repair_hold_SA", 2,
                           "SA rejected worse buffer insertion: "
                           "slack_delta={:.3e}, temp={:.3e}, prob={:.3f}, rand={:.3f}",
                           setup_slack_delta, current_temp, acceptance_prob, random_val);
                }
                
                hold_buffer_insertion_passes++;
              }

              if (!accept_change) {
                resizer_->journalRestore();
                inserted_buffer_count_ = 0;
              } else {
                resizer_->journalEnd();
              }
            }
          }
        }
      }
    }
  }
}

void RepairHold::mergeInit(Slacks& slacks)
{
  slacks[rise_index_][min_index_] = INF;
  slacks[fall_index_][min_index_] = INF;
  slacks[rise_index_][max_index_] = -INF;
  slacks[fall_index_][max_index_] = -INF;
}

void RepairHold::mergeInto(Slacks& from, Slacks& result)
{
  result[rise_index_][min_index_]
      = min(result[rise_index_][min_index_], from[rise_index_][min_index_]);
  result[fall_index_][min_index_]
      = min(result[fall_index_][min_index_], from[fall_index_][min_index_]);
  result[rise_index_][max_index_]
      = max(result[rise_index_][max_index_], from[rise_index_][max_index_]);
  result[fall_index_][max_index_]
      = max(result[fall_index_][max_index_], from[fall_index_][max_index_]);
}

void RepairHold::makeHoldDelay(Vertex* drvr,
                               PinSeq& load_pins,
                               bool loads_have_out_port,  // top level port
                               LibertyCell* buffer_cell,
                               const Point& loc)
{
  Pin* drvr_pin = drvr->pin();
  odb::dbModNet* mod_drvr_net = nullptr;  // hierarchical driver, default none
  dbNet* db_drvr_net = nullptr;           // regular flat driver

  Instance* parent = nullptr;
  if (db_network_->hasHierarchy()) {
    // get the nets on the driver pin (possibly both flat and hierarchical)
    db_network_->net(drvr_pin, db_drvr_net, mod_drvr_net);
    // Get the parent instance (owning the instance of the driver pin)
    // we will put the new buffer in that parent
    parent = db_network_->getOwningInstanceParent(drvr_pin);
    // exception case: drvr pin is a top level, fix the db_drvr_net to be
    // the lower level net. Explictly get the "flat" net.
    if (network_->isTopLevelPort(drvr_pin)) {
      db_drvr_net = db_network_->flatNet(db_network_->term(drvr_pin));
    }
  } else {
    // original flat code (which handles exception case at top level &
    // defaults to top level instance as parent).
    db_drvr_net = db_network_->staToDb(
        network_->isTopLevelPort(drvr_pin)
            ? db_network_->net(db_network_->term(drvr_pin))
            : db_network_->net(drvr_pin));
    parent = db_network_->topInstance();
  }
  Net *in_net = nullptr, *out_net = nullptr;

  if (loads_have_out_port) {
    // Verilog uses nets as ports, so the net connected to an output port has
    // to be preserved.
    // Move the driver pin over to gensym'd net.
    //
    in_net = resizer_->makeUniqueNet();
    Port* drvr_port = network_->port(drvr_pin);
    Instance* drvr_inst = network_->instance(drvr_pin);
    sta_->disconnectPin(drvr_pin);
    sta_->connectPin(drvr_inst, drvr_port, in_net);
    out_net = db_network_->dbToSta(db_drvr_net);
  } else {
    in_net = db_network_->dbToSta(db_drvr_net);
    // make the output net, put in same module as buffer
    std::string net_name = resizer_->makeUniqueNetName();
    out_net = db_network_->makeNet(net_name.c_str(), parent);
  }

  dbNet* in_net_db = db_network_->staToDb(in_net);

  // Disconnect the original drvr pin from everything (hierarchical nets
  // and flat nets).
  odb::dbITerm* drvr_pin_iterm;
  odb::dbBTerm* drvr_pin_bterm;
  odb::dbModITerm* drvr_pin_moditerm;
  db_network_->staToDb(
      drvr_pin, drvr_pin_iterm, drvr_pin_bterm, drvr_pin_moditerm);
  if (drvr_pin_iterm) {
    // disconnect the iterm from both the modnet and the dbnet
    // note we will rewire the drvr_pin to connect to the new buffer later.
    drvr_pin_iterm->disconnect();
    drvr_pin_iterm->connect(in_net_db);
  }
  if (drvr_pin_moditerm) {
    drvr_pin_moditerm->disconnect();
  }

  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);

  // drvr_pin->drvr_net->hold_buffer->net2->load_pins

  string buffer_name = resizer_->makeUniqueInstName("hold");

  // make the buffer in the driver pin's parent
  Instance* buffer
      = resizer_->makeBuffer(buffer_cell, buffer_name.c_str(), parent, loc);
  inserted_buffer_count_++;
  debugPrint(
      logger_, RSZ, "repair_hold", 3, " insert {}", network_->name(buffer));

  // wire in the buffer
  sta_->connectPin(buffer, input, in_net);
  sta_->connectPin(buffer, output, out_net);

  // Now patch in the output of the new buffer to the original hierarchical
  // net,if any, from the original driver
  if (mod_drvr_net != nullptr) {
    Pin* ip_pin = nullptr;
    Pin* op_pin = nullptr;
    resizer_->getBufferPins(buffer, ip_pin, op_pin);
    (void) ip_pin;
    if (op_pin) {
      // get the iterm of the op_pin of the buffer (a dbInst)
      // and connect to the hierarchical net.
      odb::dbITerm* iterm;
      odb::dbBTerm* bterm;
      odb::dbModITerm* moditerm;
      db_network_->staToDb(op_pin, iterm, bterm, moditerm);
      // we only need to look at the iterm, the buffer is a dbInst
      if (iterm) {
        // hook up the hierarchical net
        iterm->connect(mod_drvr_net);
      }
    }
  }

  // hook up loads to buffer
  for (const Pin* load_pin : load_pins) {
    if (resizer_->dontTouch(load_pin)) {
      continue;
    }
    dbNet* db_load_net = network_->isTopLevelPort(load_pin)
                             ? db_network_->flatNet(network_->term(load_pin))
                             : db_network_->flatNet(load_pin);
    Net* load_net = db_network_->dbToSta(db_load_net);

    if (load_net != out_net) {
      Instance* load = db_network_->instance(load_pin);
      Port* load_port = db_network_->port(load_pin);
      // record the original connections
      odb::dbModNet* original_mod_net = nullptr;
      odb::dbNet* original_flat_net = nullptr;
      db_network_->net(load_pin, original_flat_net, original_mod_net);
      (void) original_flat_net;
      // Remove all the connections on load_pin
      sta_->disconnectPin(const_cast<Pin*>(load_pin));
      // Connect it to the correct output driver net
      sta_->connectPin(load, load_port, out_net);
      // connect the original load  modnet (hierarchical net), if any,
      // on the iterm of the buffer created.
      odb::dbITerm* iterm;
      odb::dbBTerm* bterm;
      odb::dbModITerm* moditerm;
      db_network_->staToDb(load_pin, iterm, bterm, moditerm);
      if (iterm && original_mod_net) {
        iterm->connect(original_mod_net);
      }
    }
  }

  Pin* buffer_out_pin = network_->findPin(buffer, output);
  Vertex* buffer_out_vertex = graph_->pinDrvrVertex(buffer_out_pin);
  resizer_->updateParasitics();
  // Sta::checkMaxSlewCap does not force dcalc update so do it explicitly.
  sta_->findDelays(buffer_out_vertex);
  if (!checkMaxSlewCap(buffer_out_pin)
      && resizer_->resizeToTargetSlew(buffer_out_pin)) {
    resizer_->updateParasitics();
    resize_count_++;
  }
}

bool RepairHold::checkMaxSlewCap(const Pin* drvr_pin)
{
  float cap, limit, slack;
  const Corner* corner;
  const RiseFall* tr;
  sta_->checkCapacitance(
      drvr_pin, nullptr, max_, corner, tr, cap, limit, slack);
  float slack_limit_ratio = slack / limit;
  if (slack_limit_ratio < hold_slack_limit_ratio_max_) {
    return false;
  }

  Slew slew;
  sta_->checkSlew(
      drvr_pin, nullptr, max_, false, corner, tr, slew, limit, slack);
  slack_limit_ratio = slack / limit;
  if (slack_limit_ratio < hold_slack_limit_ratio_max_) {
    return false;
  }

  resizer_->checkLoadSlews(drvr_pin, 0.0, slew, limit, slack, corner);
  slack_limit_ratio = slack / limit;
  if (slack_limit_ratio < hold_slack_limit_ratio_max_) {
    return false;
  }

  return true;
}

void RepairHold::printProgress(int iteration, bool force, bool end) const
{
  const bool start = iteration == 0;

  if (start) {
    logger_->report(
        "Iteration | Resized | Buffers | Cloned Gates |   Area   |   WNS   "
        "|   TNS   | Endpoint");
    logger_->report(
        "----------------------------------------------------------------------"
        "----------------");
  }

  if (iteration % print_interval_ == 0 || force || end) {
    Slack wns;
    Vertex* worst_vertex;
    sta_->worstSlack(min_, wns, worst_vertex);
    const Slack tns = sta_->totalNegativeSlack(min_);

    std::string itr_field = fmt::format("{}", iteration);
    if (end) {
      itr_field = "final";
    }

    const double design_area = resizer_->computeDesignArea();
    const double area_growth = design_area - initial_design_area_;

    logger_->report(
        "{: >9s} | {: >7d} | {: >7d} | {: >12d} | {: >+7.1f}% | {: >7s} | {: "
        ">7s} | {}",
        itr_field,
        resize_count_,
        inserted_buffer_count_,
        cloned_gate_count_,
        area_growth / initial_design_area_ * 1e2,
        delayAsString(wns, sta_, 3),
        delayAsString(tns, sta_, 3),
        worst_vertex->name(network_));
  }

  if (end) {
    logger_->report(
        "----------------------------------------------------------------------"
        "----------------");
  }
}

}  // namespace rsz
