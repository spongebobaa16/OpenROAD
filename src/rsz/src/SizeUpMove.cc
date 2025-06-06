// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2025, The OpenROAD Authors

#include "SizeUpMove.hh"

#include <cmath>

#include "BaseMove.hh"
#include "CloneMove.hh"

namespace rsz {

using std::string;

using utl::RSZ;

using sta::ArcDelay;
using sta::DcalcAnalysisPt;
using sta::Instance;
using sta::InstancePinIterator;
using sta::LibertyCell;
using sta::LibertyCellSeq;
using sta::LibertyPort;
using sta::LoadPinIndexMap;
using sta::NetConnectedPinIterator;
using sta::Path;
using sta::PathExpanded;
using sta::Pin;
using sta::Slack;
using sta::Slew;

using sta::MinMax;

bool SizeUpMove::doMove(const Path* drvr_path,
                        int drvr_index,
                        Slack drvr_slack,
                        PathExpanded* expanded,
                        float setup_slack_margin)
{
  Pin* drvr_pin = drvr_path->pin(this);
  Instance* drvr = network_->instance(drvr_pin);
  const DcalcAnalysisPt* dcalc_ap = drvr_path->dcalcAnalysisPt(sta_);
  const float load_cap = graph_delay_calc_->loadCap(drvr_pin, dcalc_ap);
  const int in_index = drvr_index - 1;
  const Path* in_path = expanded->path(in_index);
  Pin* in_pin = in_path->pin(sta_);
  LibertyPort* in_port = network_->libertyPort(in_pin);

  // We always size the cloned gates for some reason, but it would be good if we
  // also down-sized here instead since we might want smaller original.
  if (!resizer_->dontTouch(drvr)
      || resizer_->clone_move->hasPendingMoves(drvr)) {
    float prev_drive;
    if (drvr_index >= 2) {
      const int prev_drvr_index = drvr_index - 2;
      const Path* prev_drvr_path = expanded->path(prev_drvr_index);
      Pin* prev_drvr_pin = prev_drvr_path->pin(sta_);
      prev_drive = 0.0;
      LibertyPort* prev_drvr_port = network_->libertyPort(prev_drvr_pin);
      if (prev_drvr_port) {
        prev_drive = prev_drvr_port->driveResistance();
      }
    } else {
      prev_drive = 0.0;
    }
    Slack worst_slack, prev_worst_slack;
    Vertex* worst_vertex;
    sta_->worstSlack(resizer_->max_, prev_worst_slack, worst_vertex);
    float prev_tns = sta_->totalNegativeSlack(resizer_->max_);
    float curr_tns;
    constexpr int digits = 3;

    LibertyPort* drvr_port = network_->libertyPort(drvr_pin);
    LibertyCell* original_cell = drvr_port->libertyCell();
    debugPrint(logger_, RSZ, "upsizeMove", 1, "Original cell: {}", original_cell->name());
    // Get all swappable cells for this gate
    LibertyCellSeq swappable_cells = resizer_->getSwappableCells(original_cell);
    
    if (!swappable_cells.empty()) {
      // Record baseline TNS
      float baseline_tns = sta_->totalNegativeSlack(resizer_->max_);
      LibertyCell* best_cell = nullptr;
      float best_tns = baseline_tns;
      
      debugPrint(logger_, RSZ, "upsizeMove", 2, 
                 "Testing {} candidate cells for {} (baseline TNS: {})",
                 swappable_cells.size(),
                 network_->pathName(drvr_pin),
                 delayAsString(baseline_tns, sta_, 3));
      
      // Try each candidate cell
      for (LibertyCell* candidate : swappable_cells) {
        if (!resizer_->dontTouch(drvr) && replaceCell(drvr, candidate)) {
          // Update parasitics and timing to get accurate TNS
          resizer_->updateParasitics();
          sta_->findRequireds();
          
          float candidate_tns = sta_->totalNegativeSlack(resizer_->max_);
          
          debugPrint(logger_, RSZ, "upsizeMove", 3,
                     "Candidate {} -> {}: TNS {} -> {} (improvement: {})",
                     original_cell->name(), 
                     candidate->name(),
                     delayAsString(baseline_tns, sta_, 3),
                     delayAsString(candidate_tns, sta_, 3),
                     delayAsString(candidate_tns - baseline_tns, sta_, 3));
          
          // Check if this is the best improvement so far
          if (candidate_tns > best_tns) {  // More positive TNS is better
            best_tns = candidate_tns;
            best_cell = candidate;
          }
          
          // Restore original cell for next test (unless it's the last iteration)
          replaceCell(drvr, original_cell);
        }
      }
      
      // If we found a better cell, use it
      if (best_cell && best_tns > baseline_tns) {
        if (!resizer_->dontTouch(drvr) && replaceCell(drvr, best_cell)) {
          // Update parasitics and required times to ensure consistent TNS calculation
          resizer_->updateParasitics();
          sta_->findRequireds();
          
          sta_->worstSlack(resizer_->max_, worst_slack, worst_vertex);
          curr_tns = sta_->totalNegativeSlack(resizer_->max_);

          debugPrint(logger_, RSZ, "upsizeMove", 1, "Best: upsizing move accepted "
                       "{} {} -> {}, wns from {} to {} and "
                       "tns from {} to {}",
                       network_->pathName(drvr_pin),
                       original_cell->name(),
                       best_cell->name(),
                       delayAsString(prev_worst_slack, sta_, digits),
                       delayAsString(worst_slack, sta_, digits),
                       delayAsString(prev_tns, sta_, digits),
                       delayAsString(curr_tns, sta_, digits));
          debugPrint(logger_,
                     RSZ,
                     "opt_moves",
                     1,
                     "ACCEPT size_up {} {} -> {}",
                     network_->pathName(drvr_pin),
                     original_cell->name(),
                     best_cell->name());
          debugPrint(logger_,
                     RSZ,
                     "repair_setup",
                     3,
                     "size_up {} {} -> {}",
                     network_->pathName(drvr_pin),
                     original_cell->name(),
                     best_cell->name());
          addMove(drvr);
          return true;
        }
      }
    }
    
  //   // Fallback to original upsizeCell logic if no improvement found
  //   LibertyCell* upsize
  //       = upsizeCell(in_port, drvr_port, load_cap, prev_drive, dcalc_ap);
    
    
  //   if (upsize && !resizer_->dontTouch(drvr) && replaceCell(drvr, upsize)) {
  //     // Update parasitics and required times to ensure consistent TNS calculation
  //     resizer_->updateParasitics();
  //     sta_->findRequireds();
      
  //     sta_->worstSlack(resizer_->max_, worst_slack, worst_vertex);
  //     curr_tns = sta_->totalNegativeSlack(resizer_->max_);

  //     debugPrint(logger_, RSZ, "upsizeMove", 1, "Better: upsizing move accepted "
  //                  "{} {} -> {}, wns from {} to {} and "
  //                  "tns from {} to {}",
  //                  network_->pathName(drvr_pin),
  //                  drvr_port->libertyCell()->name(),
  //                  upsize->name(),
  //                  delayAsString(prev_worst_slack, sta_, digits),
  //                  delayAsString(worst_slack, sta_, digits),
  //                  delayAsString(prev_tns, sta_, digits),
  //                  delayAsString(curr_tns, sta_, digits));
  //     debugPrint(logger_,
  //                RSZ,
  //                "opt_moves",
  //                1,
  //                "ACCEPT size_up {} {} -> {}",
  //                network_->pathName(drvr_pin),
  //                drvr_port->libertyCell()->name(),
  //                upsize->name());
  //     debugPrint(logger_,
  //                RSZ,
  //                "repair_setup",
  //                3,
  //                "size_up {} {} -> {}",
  //                network_->pathName(drvr_pin),
  //                drvr_port->libertyCell()->name(),
  //                upsize->name());
  //     addMove(drvr);
  //     return true;
  //   }
  //   debugPrint(logger_,
  //              RSZ,
  //              "opt_moves",
  //              3,
  //              "REJECT size_up {} {}",
  //              network_->pathName(drvr_pin),
  //              drvr_port->libertyCell()->name());
  }

  return false;
}

// namespace rsz
}  // namespace rsz
