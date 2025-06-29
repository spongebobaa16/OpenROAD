// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "defout_impl.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "odb/db.h"
#include "odb/dbMap.h"
#include "odb/dbWireCodec.h"
#include "utl/Logger.h"
#include "utl/ScopedTemporaryFile.h"
namespace odb {

namespace {

std::string getPinName(dbBTerm* bterm)
{
  return bterm->getName();
}

std::string getPinName(dbITerm* iterm)
{
  return iterm->getMTerm()->getName();
}

static const int max_name_length = 256;

template <typename T>
std::vector<T*> sortedSet(dbSet<T>& to_sort)
{
  std::vector<T*> sorted(to_sort.begin(), to_sort.end());
  std::sort(sorted.begin(), sorted.end(), [](T* a, T* b) {
    return a->getName() < b->getName();
  });
  return sorted;
}

const char* defOrient(const dbOrientType& orient)
{
  switch (orient.getValue()) {
    case dbOrientType::R0:
      return "N";

    case dbOrientType::R90:
      return "W";

    case dbOrientType::R180:
      return "S";

    case dbOrientType::R270:
      return "E";

    case dbOrientType::MY:
      return "FN";

    case dbOrientType::MYR90:
      return "FE";

    case dbOrientType::MX:
      return "FS";

    case dbOrientType::MXR90:
      return "FW";
  }

  return "N";
}

const char* defSigType(const dbSigType& type)
{
  return type.getString();
}
const char* defIoType(const dbIoType& type)
{
  return type.getString();
}

}  // namespace

void defout_impl::selectNet(dbNet* net)
{
  if (!net) {
    return;
  }
  _select_net_list.push_back(net);
}



bool defout_impl::writeBlock_Pl(dbBlock* block, const char* def_file)
{
  if (!_select_net_list.empty()) {
    _select_net_map = new dbMap<dbNet, char>(block->getNets());
    std::list<dbNet*>::iterator sitr;
    for (sitr = _select_net_list.begin(); sitr != _select_net_list.end();
         ++sitr) {
      dbNet* net = *sitr;
      (*_select_net_map)[net] = 1;
      if (net->isSpecial() || net->isMark_1ed()) {
        continue;
      }
      if (!_select_inst_map) {
        _select_inst_map = new dbMap<dbInst, char>(block->getInsts());
      }
      dbSet<dbITerm> iterms = net->getITerms();
      dbSet<dbITerm>::iterator titr;
      for (titr = iterms.begin(); titr != iterms.end(); ++titr) {
        dbInst* inst = (*titr)->getInst();
        (*_select_inst_map)[inst] = 1;
      }
    }
  }
  if (!_select_inst_list.empty()) {
    if (!_select_inst_map) {
      _select_inst_map = new dbMap<dbInst, char>(block->getInsts());
    }
    std::list<dbInst*>::iterator xitr;
    for (xitr = _select_inst_list.begin(); xitr != _select_inst_list.end();
         ++xitr) {
      dbInst* inst = *xitr;
      (*_select_inst_map)[inst] = 1;
    }
  }

  _dist_factor
      = (double) block->getDefUnits() / (double) block->getDbUnitsPerMicron();
  utl::FileHandler fileHandler(def_file);
  _out = fileHandler.getFile();



  // By default C File*'s are line buffered which means they get dumped on every
  // newline, which is nominally pretty expensive. This makes it so that the
  // writes are buffered according to the block size which on modern systems can
  // be as much as 16kb. DEF's have a lot of newlines, and are large in size
  // which makes writing them really slow with line buffering.
  //
  // The following lines enable IO buffering based on disk block size.
  struct stat stats;
  fstat(fileno(_out), &stats);
  setvbuf(_out, nullptr, _IOFBF, stats.st_blksize);

  writeBTerms_Pl(block);

  fprintf(_out, "CELLS\n");
  
  writeInsts_Pl(block);

  return true;
}

bool defout_impl::writeBlock(dbBlock* block, const char* def_file)
{
  if (!_select_net_list.empty()) {
    _select_net_map = new dbMap<dbNet, char>(block->getNets());
    std::list<dbNet*>::iterator sitr;
    for (sitr = _select_net_list.begin(); sitr != _select_net_list.end();
         ++sitr) {
      dbNet* net = *sitr;
      (*_select_net_map)[net] = 1;
      if (net->isSpecial() || net->isMark_1ed()) {
        continue;
      }
      if (!_select_inst_map) {
        _select_inst_map = new dbMap<dbInst, char>(block->getInsts());
      }
      dbSet<dbITerm> iterms = net->getITerms();
      dbSet<dbITerm>::iterator titr;
      for (titr = iterms.begin(); titr != iterms.end(); ++titr) {
        dbInst* inst = (*titr)->getInst();
        (*_select_inst_map)[inst] = 1;
      }
    }
  }
  if (!_select_inst_list.empty()) {
    if (!_select_inst_map) {
      _select_inst_map = new dbMap<dbInst, char>(block->getInsts());
    }
    std::list<dbInst*>::iterator xitr;
    for (xitr = _select_inst_list.begin(); xitr != _select_inst_list.end();
         ++xitr) {
      dbInst* inst = *xitr;
      (*_select_inst_map)[inst] = 1;
    }
  }

  _dist_factor
      = (double) block->getDefUnits() / (double) block->getDbUnitsPerMicron();
  utl::FileHandler fileHandler(def_file);
  _out = fileHandler.getFile();

  if (_out == nullptr) {
    _logger->warn(
        utl::ODB, 172, "Cannot open DEF file ({}) for writing", def_file);
    return false;
  }

  // By default C File*'s are line buffered which means they get dumped on every
  // newline, which is nominally pretty expensive. This makes it so that the
  // writes are buffered according to the block size which on modern systems can
  // be as much as 16kb. DEF's have a lot of newlines, and are large in size
  // which makes writing them really slow with line buffering.
  //
  // The following lines enable IO buffering based on disk block size.
  struct stat stats;
  fstat(fileno(_out), &stats);
  setvbuf(_out, nullptr, _IOFBF, stats.st_blksize);

  if (_version == defout::DEF_5_3) {
    fprintf(_out, "VERSION 5.3 ;\n");
  } else if (_version == defout::DEF_5_4) {
    fprintf(_out, "VERSION 5.4 ;\n");
  } else if (_version == defout::DEF_5_5) {
    fprintf(_out, "VERSION 5.5 ;\n");
  } else if (_version == defout::DEF_5_6) {
    fprintf(_out, "VERSION 5.6 ;\n");
  } else if (_version == defout::DEF_5_7) {
    fprintf(_out, "VERSION 5.7 ;\n");
  } else if (_version == defout::DEF_5_8) {
    fprintf(_out, "VERSION 5.8 ;\n");
  }
  if (_version < defout::DEF_5_6) {
    fprintf(_out, "NAMESCASESENSITIVE ON ;\n");
  }
  char hd = block->getHierarchyDelimiter();

  if (hd == 0) {
    hd = '|';
  }

  fprintf(_out, "DIVIDERCHAR \"%c\" ;\n", hd);

  char left_bus, right_bus;
  block->getBusDelimiters(left_bus, right_bus);

  if ((left_bus == 0) || (right_bus == 0)) {
    left_bus = '[';
    right_bus = ']';
  }

  fprintf(_out, "BUSBITCHARS \"%c%c\" ;\n", left_bus, right_bus);

  std::string bname = block->getName();
  fprintf(_out, "DESIGN %s ;\n", bname.c_str());

  fprintf(_out, "UNITS DISTANCE MICRONS %d ;\n", block->getDefUnits());

  writePropertyDefinitions(block);

  Polygon die_area = block->getDieAreaPolygon();

  if (die_area.isRect()) {
    Rect r = die_area.getEnclosingRect();
    int x1 = defdist(r.xMin());
    int y1 = defdist(r.yMin());
    int x2 = defdist(r.xMax());
    int y2 = defdist(r.yMax());

    if ((x1 != 0) || (y1 != 0) || (x2 != 0) || (y2 != 0)) {
      fprintf(_out, "DIEAREA ( %d %d ) ( %d %d ) ;\n", x1, y1, x2, y2);
    }
  } else {
    fprintf(_out, "DIEAREA ");
    std::vector<odb::Point> points = die_area.getPoints();
    // ODB ends polygons with a copy of 0 index vertex, in DEF there
    // is an implicit rule that the last vertex is connected to the
    // 0th index. So we skip the last point.
    for (int i = 0; i < points.size() - 1; i++) {
      int x = defdist(points[i].x());
      int y = defdist(points[i].y());
      fprintf(_out, "( %d %d ) ", x, y);
    }
    fprintf(_out, ";\n");
  }

  writeRows(block);
  writeTracks(block);
  writeGCells(block);
  writeVias(block);
  writeNonDefaultRules(block);
  writeRegions(block);
  if (_version == defout::DEF_5_8) {
    writeComponentMaskShift(block);
  }
  writeInsts(block);
  writeBTerms(block);
  writePinProperties(block);
  writeBlockages(block);
  writeFills(block);
  writeNets(block);
  writeGroups(block);
  writeScanChains(block);

  fprintf(_out, "END DESIGN\n");
  {
    delete _select_net_map;
  }
  {
    delete _select_inst_map;
  }
  return true;
}

void defout_impl::writeRows(dbBlock* block)
{
  dbSet<dbRow> rows = block->getRows();
  dbSet<dbRow>::iterator itr;

  for (itr = rows.begin(); itr != rows.end(); ++itr) {
    dbRow* row = *itr;
    std::string n = row->getName();
    Point origin = row->getOrigin();
    int s = row->getSpacing();
    int c = row->getSiteCount();
    dbSite* site = row->getSite();
    std::string sn = site->getName();
    const char* o = defOrient(row->getOrient());

    fprintf(_out,
            "ROW %s %s %d %d %s ",
            n.c_str(),
            sn.c_str(),
            defdist(origin.x()),
            defdist(origin.y()),
            o);

    if (row->getDirection() == dbRowDir::VERTICAL) {
      fprintf(_out, "DO 1 BY %d STEP 0 %d", c, defdist(s));
    } else {
      fprintf(_out, "DO %d BY 1 STEP %d 0", c, defdist(s));
    }

    if (hasProperties(row, ROW)) {
      fprintf(_out, " + PROPERTY ");
      writeProperties(row);
    }

    fprintf(_out, " ;\n");
  }
}

void defout_impl::writeTracks(dbBlock* block)
{
  dbSet<dbTrackGrid> grids = block->getTrackGrids();
  dbSet<dbTrackGrid>::iterator itr;

  for (itr = grids.begin(); itr != grids.end(); ++itr) {
    dbTrackGrid* grid = *itr;
    dbTechLayer* layer = grid->getTechLayer();

    std::string lname;
    if (_use_alias && layer->hasAlias()) {
      lname = layer->getAlias();
    } else {
      lname = layer->getName();
    }

    for (int i = 0; i < grid->getNumGridPatternsX(); ++i) {
      int orgX, count, step, firstmask;
      bool samemask;
      grid->getGridPatternX(i, orgX, count, step, firstmask, samemask);
      fprintf(_out,
              "TRACKS X %d DO %d STEP %d",
              defdist(orgX),
              count,
              defdist(step));
      if (firstmask != 0) {
        fprintf(_out, " MASK %d", firstmask);
        if (samemask) {
          fprintf(_out, " SAMEMASK");
        }
      }
      fprintf(_out, " LAYER %s ;\n", lname.c_str());
    }

    for (int i = 0; i < grid->getNumGridPatternsY(); ++i) {
      int orgY, count, step, firstmask;
      bool samemask;
      grid->getGridPatternY(i, orgY, count, step, firstmask, samemask);
      fprintf(_out,
              "TRACKS Y %d DO %d STEP %d",
              defdist(orgY),
              count,
              defdist(step));
      if (firstmask != 0) {
        fprintf(_out, " MASK %d", firstmask);
        if (samemask) {
          fprintf(_out, " SAMEMASK");
        }
      }
      fprintf(_out, " LAYER %s ;\n", lname.c_str());
    }
  }
}

void defout_impl::writeGCells(dbBlock* block)
{
  dbGCellGrid* grid = block->getGCellGrid();

  if (grid == nullptr) {
    return;
  }

  int i;

  for (i = 0; i < grid->getNumGridPatternsX(); ++i) {
    int orgX, count, step;
    grid->getGridPatternX(i, orgX, count, step);
    fprintf(_out,
            "GCELLGRID X %d DO %d STEP %d ;\n",
            defdist(orgX),
            count,
            defdist(step));
  }

  for (i = 0; i < grid->getNumGridPatternsY(); ++i) {
    int orgY, count, step;
    grid->getGridPatternY(i, orgY, count, step);
    fprintf(_out,
            "GCELLGRID Y %d DO %d STEP %d ;\n",
            defdist(orgY),
            count,
            defdist(step));
  }
}

void defout_impl::writeVias(dbBlock* block)
{
  dbSet<dbVia> vias = block->getVias();

  if (vias.size() == 0) {
    return;
  }

  dbSet<dbVia>::iterator itr;
  uint cnt = 0;

  for (itr = vias.begin(); itr != vias.end(); ++itr) {
    dbVia* via = *itr;

    if ((_version >= defout::DEF_5_6) && via->isViaRotated()) {
      continue;
    }

    ++cnt;
  }

  fprintf(_out, "VIAS %u ;\n", cnt);

  for (itr = vias.begin(); itr != vias.end(); ++itr) {
    dbVia* via = *itr;

    if ((_version >= defout::DEF_5_6) && via->isViaRotated()) {
      continue;
    }

    writeVia(via);
  }

  fprintf(_out, "END VIAS\n");
}

void defout_impl::writeVia(dbVia* via)
{
  std::string vname = via->getName();
  fprintf(_out, "    - %s", vname.c_str());
  dbTechViaGenerateRule* rule = via->getViaGenerateRule();

  if ((_version >= defout::DEF_5_6) && via->hasParams() && (rule != nullptr)) {
    std::string rname = rule->getName();
    fprintf(_out, " + VIARULE %s", rname.c_str());

    const dbViaParams P = via->getViaParams();

    fprintf(_out,
            " + CUTSIZE %d %d ",
            defdist(P.getXCutSize()),
            defdist(P.getYCutSize()));
    std::string top = P.getTopLayer()->getName();
    std::string bot = P.getBottomLayer()->getName();
    std::string cut = P.getCutLayer()->getName();
    fprintf(_out, " + LAYERS %s %s %s ", bot.c_str(), cut.c_str(), top.c_str());
    fprintf(_out,
            " + CUTSPACING %d %d ",
            defdist(P.getXCutSpacing()),
            defdist(P.getYCutSpacing()));
    fprintf(_out,
            " + ENCLOSURE %d %d %d %d ",
            defdist(P.getXBottomEnclosure()),
            defdist(P.getYBottomEnclosure()),
            defdist(P.getXTopEnclosure()),
            defdist(P.getYTopEnclosure()));

    if ((P.getNumCutRows() != 1) || (P.getNumCutCols() != 1)) {
      fprintf(_out, " + ROWCOL %d %d ", P.getNumCutRows(), P.getNumCutCols());
    }

    if ((P.getXOrigin() != 0) || (P.getYOrigin() != 0)) {
      fprintf(_out,
              " + ORIGIN %d %d ",
              defdist(P.getXOrigin()),
              defdist(P.getYOrigin()));
    }

    if ((P.getXTopOffset() != 0) || (P.getYTopOffset() != 0)
        || (P.getXBottomOffset() != 0) || (P.getYBottomOffset() != 0)) {
      fprintf(_out,
              " + OFFSET %d %d %d %d ",
              defdist(P.getXBottomOffset()),
              defdist(P.getYBottomOffset()),
              defdist(P.getXTopOffset()),
              defdist(P.getYTopOffset()));
    }

    std::string pname = via->getPattern();
    if (strcmp(pname.c_str(), "") != 0) {
      fprintf(_out, " + PATTERNNAME %s", pname.c_str());
    }
  } else {
    std::string pname = via->getPattern();
    if (strcmp(pname.c_str(), "") != 0) {
      fprintf(_out, " + PATTERNNAME %s", pname.c_str());
    }

    int i = 0;
    dbSet<dbBox> boxes = via->getBoxes();
    dbSet<dbBox>::iterator bitr;

    for (bitr = boxes.begin(); bitr != boxes.end(); ++bitr) {
      dbBox* box = *bitr;
      dbTechLayer* layer = box->getTechLayer();
      std::string lname;
      if (_use_alias && layer->hasAlias()) {
        lname = layer->getAlias();
      } else {
        lname = layer->getName();
      }
      int x1 = defdist(box->xMin());
      int y1 = defdist(box->yMin());
      int x2 = defdist(box->xMax());
      int y2 = defdist(box->yMax());

      if ((++i & 7) == 0) {
        fprintf(_out, "\n      ");
      }

      fprintf(_out,
              " + RECT %s ( %d %d ) ( %d %d )",
              lname.c_str(),
              x1,
              y1,
              x2,
              y2);
    }
  }

  fprintf(_out, " ;\n");
}

void defout_impl::writeComponentMaskShift(dbBlock* block)
{
  const std::vector<dbTechLayer*> layers = block->getComponentMaskShift();

  if (layers.empty()) {
    return;
  }

  fprintf(_out, "COMPONENTMASKSHIFT ");
  for (dbTechLayer* layer : layers) {
    fprintf(_out, "%s ", layer->getConstName());
  }
  fprintf(_out, ";\n");
}

void defout_impl::writeInsts(dbBlock* block)
{
  dbSet<dbInst> insts = block->getInsts();

  fprintf(_out, "COMPONENTS %u ;\n", insts.size());

  // Sort the components for consistent output
  for (dbInst* inst : sortedSet(insts)) {
    if (_select_inst_map && !(*_select_inst_map)[inst]) {
      continue;
    }
    writeInst(inst);
  }

  fprintf(_out, "END COMPONENTS\n");
}

void defout_impl::writeInsts_Pl(dbBlock* block)
{
  dbSet<dbInst> insts = block->getInsts();


  // Sort the components for consistent output
  for (dbInst* inst : sortedSet(insts)) {
    if (_select_inst_map && !(*_select_inst_map)[inst]) {
      continue;
    }
    writeInst_Pl(inst);
  }

}

void defout_impl::writeNonDefaultRules(dbBlock* block)
{
  dbSet<dbTechNonDefaultRule> rules = block->getNonDefaultRules();

  if (rules.empty()) {
    return;
  }

  fprintf(_out, "NONDEFAULTRULES %u ;\n", rules.size());

  dbSet<dbTechNonDefaultRule>::iterator itr;

  for (itr = rules.begin(); itr != rules.end(); ++itr) {
    dbTechNonDefaultRule* rule = *itr;
    writeNonDefaultRule(rule);
  }

  fprintf(_out, "END NONDEFAULTRULES\n");
}

void defout_impl::writeNonDefaultRule(dbTechNonDefaultRule* rule)
{
  std::string name = rule->getName();
  fprintf(_out, "    - %s\n", name.c_str());

  if (rule->getHardSpacing()) {
    fprintf(_out, "      + HARDSPACING\n");
  }

  std::vector<dbTechLayerRule*> layer_rules;
  rule->getLayerRules(layer_rules);

  std::vector<dbTechLayerRule*>::iterator litr;
  for (litr = layer_rules.begin(); litr != layer_rules.end(); ++litr) {
    writeLayerRule(*litr);
  }

  std::vector<dbTechVia*> use_vias;
  rule->getUseVias(use_vias);

  std::vector<dbTechVia*>::iterator uvitr;
  for (uvitr = use_vias.begin(); uvitr != use_vias.end(); ++uvitr) {
    dbTechVia* via = *uvitr;
    std::string vname = via->getName();
    fprintf(_out, "      + VIA %s\n", vname.c_str());
  }

  std::vector<dbTechViaGenerateRule*> use_rules;
  rule->getUseViaRules(use_rules);

  std::vector<dbTechViaGenerateRule*>::iterator uvritr;
  for (uvritr = use_rules.begin(); uvritr != use_rules.end(); ++uvritr) {
    dbTechViaGenerateRule* rule = *uvritr;
    std::string rname = rule->getName();
    fprintf(_out, "      + VIARULE %s\n", rname.c_str());
  }

  dbTech* tech = rule->getDb()->getTech();
  dbSet<dbTechLayer> layers = tech->getLayers();
  dbSet<dbTechLayer>::iterator layitr;

  for (layitr = layers.begin(); layitr != layers.end(); ++layitr) {
    dbTechLayer* layer = *layitr;
    int count;

    if (rule->getMinCuts(layer, count)) {
      std::string lname = layer->getName();
      fprintf(_out, "      + MINCUTS %s %d\n", lname.c_str(), count);
    }
  }

  if (hasProperties(rule, NONDEFAULTRULE)) {
    fprintf(_out, "    + PROPERTY ");
    writeProperties(rule);
  }

  fprintf(_out, "    ;\n");
}

void defout_impl::writeLayerRule(dbTechLayerRule* rule)
{
  dbTechLayer* layer = rule->getLayer();
  std::string name = layer->getName();

  fprintf(_out, "      + LAYER %s", name.c_str());

  fprintf(_out, " WIDTH %d", defdist(rule->getWidth()));

  if (rule->getSpacing()) {
    fprintf(_out, " SPACING %d", defdist(rule->getSpacing()));
  }

  if (rule->getWireExtension() != 0.0) {
    fprintf(_out, " WIREEXTENSION %d", defdist(rule->getWireExtension()));
  }

  fprintf(_out, "\n");
}

void defout_impl::writeInst(dbInst* inst)
{
  dbMaster* master = inst->getMaster();
  std::string mname = master->getName();

  if (_use_net_inst_ids) {
    if (_use_master_ids) {
      fprintf(_out, "    - I%u M%u", inst->getId(), master->getMasterId());
    } else {
      fprintf(_out, "    - I%u %s", inst->getId(), mname.c_str());
    }
  } else {
    std::string iname = inst->getName();
    if (_use_master_ids) {
      fprintf(_out, "    - %s M%u", iname.c_str(), master->getMasterId());
    } else {
      fprintf(_out, "    - %s %s", iname.c_str(), mname.c_str());
    }
  }

  dbSourceType source = inst->getSourceType();

  switch (source.getValue()) {
    case dbSourceType::NONE:
      break;

    case dbSourceType::NETLIST:
      fprintf(_out, " + SOURCE NETLIST");
      break;

    case dbSourceType::DIST:
      fprintf(_out, " + SOURCE DIST");
      break;

    case dbSourceType::USER:
      fprintf(_out, " + SOURCE USER");
      break;

    case dbSourceType::TIMING:
      fprintf(_out, " + SOURCE TIMING");
      break;

    case dbSourceType::TEST:
      break;
  }

  int x, y;
  inst->getLocation(x, y);
  x = defdist(x);
  y = defdist(y);

  const char* orient = defOrient(inst->getOrient());
  dbPlacementStatus status = inst->getPlacementStatus();

  switch (status.getValue()) {
    case dbPlacementStatus::NONE:
      break;

    case dbPlacementStatus::UNPLACED: {
      fprintf(_out, " + UNPLACED");
      break;
    }

    case dbPlacementStatus::SUGGESTED:
    case dbPlacementStatus::PLACED: {
      fprintf(_out, " + PLACED ( %d %d ) %s", x, y, orient);
      break;
    }

    case dbPlacementStatus::LOCKED:
    case dbPlacementStatus::FIRM: {
      fprintf(_out, " + FIXED ( %d %d ) %s", x, y, orient);
      break;
    }

    case dbPlacementStatus::COVER: {
      fprintf(_out, " + COVER ( %d %d ) %s", x, y, orient);
      break;
    }
  }

  if (inst->getWeight() != 0) {
    fprintf(_out, " + WEIGHT %d", inst->getWeight());
  }

  dbRegion* region = inst->getRegion();

  if (region) {
    if (!region->getBoundaries().empty()) {
      std::string rname = region->getName();
      fprintf(_out, " + REGION %s", rname.c_str());
    }
  }

  if (hasProperties(inst, COMPONENT)) {
    fprintf(_out, " + PROPERTY ");
    writeProperties(inst);
  }

  if (_version >= defout::DEF_5_6) {
    dbBox* box = inst->getHalo();

    if (box) {
      int left = defdist(box->xMin());
      int bottom = defdist(box->yMin());
      int right = defdist(box->xMax());
      int top = defdist(box->yMax());

      fprintf(_out, " + HALO %d %d %d %d", left, bottom, right, top);
    }
  }

  fprintf(_out, " ;\n");
}



void defout_impl::writeInst_Pl(dbInst* inst)
{
  dbMaster* master = inst->getMaster();
  std::string mname = master->getName();

  if (_use_net_inst_ids) {
      fprintf(_out, "I%u", inst->getId());
    
  } else {
    std::string iname = inst->getName();
    fprintf(_out, "%s", iname.c_str());  
  }



  int x, y;
  inst->getLocation(x, y);
  x = defdist(x);
  y = defdist(y);

  const char* orient = defOrient(inst->getOrient());
  dbPlacementStatus status = inst->getPlacementStatus();

  switch (status.getValue()) {
    case dbPlacementStatus::NONE:
      break;

    case dbPlacementStatus::UNPLACED: {
      break;
    }

    case dbPlacementStatus::SUGGESTED:
    case dbPlacementStatus::PLACED: {
      fprintf(_out, " %d %d : %s\n", x, y, orient);
      break;
    }

    case dbPlacementStatus::LOCKED:
    case dbPlacementStatus::FIRM: {
      fprintf(_out, " %d %d : %s\n", x, y, orient);
      break;
    }

    case dbPlacementStatus::COVER: {
      fprintf(_out, " %d %d : %s\n", x, y, orient);
      break;
    }
  }

}

void defout_impl::writeBTerms(dbBlock* block)
{
  dbSet<dbBTerm> bterms = block->getBTerms();

  if (bterms.size() == 0) {
    return;
  }

  uint n = 0;

  dbSet<dbBTerm>::iterator itr;

  for (itr = bterms.begin(); itr != bterms.end(); ++itr) {
    dbBTerm* bterm = *itr;
    dbNet* net = bterm->getNet();

    if (net && _select_net_map && !(*_select_net_map)[net]) {
      continue;
    }

    ++n;
  }

  fprintf(_out, "PINS %u ;\n", n);

  for (dbBTerm* bterm : sortedSet(bterms)) {
    dbNet* net = bterm->getNet();
    if (net && _select_net_map && !(*_select_net_map)[net]) {
      continue;
    }
    writeBTerm(bterm);
  }

  fprintf(_out, "END PINS\n");
}


void defout_impl::writeBTerms_Pl(dbBlock* block)
{
  dbSet<dbBTerm> bterms = block->getBTerms();

  if (bterms.size() == 0) {
    return;
  }

  uint n = 0;

  dbSet<dbBTerm>::iterator itr;

  for (itr = bterms.begin(); itr != bterms.end(); ++itr) {
    dbBTerm* bterm = *itr;
    dbNet* net = bterm->getNet();

    if (net && _select_net_map && !(*_select_net_map)[net]) {
      continue;
    }

    ++n;
  }


  for (dbBTerm* bterm : sortedSet(bterms)) {
    dbNet* net = bterm->getNet();
    if (net && _select_net_map && !(*_select_net_map)[net]) {
      continue;
    }
    writeBTerm_Pl(bterm);
  }

}





void defout_impl::writeRegions(dbBlock* block)
{
  dbSet<dbRegion> regions = block->getRegions();

  uint cnt = 0;
  dbSet<dbRegion>::iterator itr;

  for (itr = regions.begin(); itr != regions.end(); ++itr) {
    dbRegion* region = *itr;

    dbSet<dbBox> boxes = region->getBoundaries();

    if (!boxes.empty()) {
      ++cnt;
    }
  }

  if (cnt == 0) {
    return;
  }

  fprintf(_out, "REGIONS %u ;\n", cnt);

  for (itr = regions.begin(); itr != regions.end(); ++itr) {
    dbRegion* region = *itr;

    dbSet<dbBox> boxes = region->getBoundaries();

    if (boxes.empty()) {
      continue;
    }

    std::string name = region->getName();
    fprintf(_out, "    - %s", name.c_str());

    dbSet<dbBox>::iterator bitr;
    int cnt = 0;

    for (bitr = boxes.begin(); bitr != boxes.end(); ++bitr, ++cnt) {
      dbBox* box = *bitr;

      if ((cnt & 0x3) == 0x3) {
        fprintf(_out, "\n        ");
      }

      fprintf(_out,
              " ( %d %d ) ( %d %d )",
              defdist(box->xMin()),
              defdist(box->yMin()),
              defdist(box->xMax()),
              defdist(box->yMax()));
    }

    switch ((dbRegionType::Value) region->getRegionType()) {
      case dbRegionType::INCLUSIVE:
        break;

      case dbRegionType::EXCLUSIVE:
        fprintf(_out, " + TYPE FENCE");
        break;

      case dbRegionType::SUGGESTED:
        fprintf(_out, " + TYPE GUIDE");
        break;
    }

    if (hasProperties(region, REGION)) {
      fprintf(_out, " + PROPERTY ");
      writeProperties(region);
    }

    fprintf(_out, " ;\n");
  }

  fprintf(_out, "END REGIONS\n");
}

void defout_impl::writeGroups(dbBlock* block)
{
  auto groups = block->getGroups();
  uint cnt = 0;
  for (auto group : groups) {
    if (!group->getInsts().empty()) {
      cnt++;
    }
  }
  if (cnt == 0) {
    return;
  }
  fprintf(_out, "GROUPS %u ;\n", cnt);

  for (auto group : groups) {
    if (group->getInsts().empty()) {
      continue;
    }
    std::string name = group->getName();
    fprintf(_out, "    - %s", name.c_str());

    dbSet<dbInst> insts = group->getInsts();
    dbSet<dbInst>::iterator iitr;
    cnt = 0;

    for (iitr = insts.begin(); iitr != insts.end(); ++iitr, ++cnt) {
      dbInst* inst = *iitr;

      if ((cnt & 0x3) == 0x3) {
        fprintf(_out, "\n        ");
      }

      std::string name = inst->getName();

      fprintf(_out, " %s", name.c_str());
    }

    dbRegion* parent = group->getRegion();

    // The semantic is: if the parent region has boundaries then it is a DEF
    // region.
    if (parent) {
      dbSet<dbBox> rboxes = parent->getBoundaries();

      if (!rboxes.empty()) {
        std::string rname = parent->getName();
        fprintf(_out, " + REGION %s", rname.c_str());
      }
    }

    if (hasProperties(group, GROUP)) {
      fprintf(_out, " + PROPERTY ");
      writeProperties(group);
    }

    fprintf(_out, " ;\n");
  }

  fprintf(_out, "END GROUPS\n");
}

void defout_impl::writeScanChains(dbBlock* block)
{
  dbDft* dft = block->getDft();
  dbSet<dbScanChain> scan_chains = dft->getScanChains();
  if (scan_chains.empty()) {
    // If we don't have scan chains we have nothing to print
    return;
  }
  fprintf(_out, "\nSCANCHAINS %d ;\n\n", scan_chains.size());

  for (dbScanChain* scan_chain : dft->getScanChains()) {
    dbSet<dbScanPartition> scan_partitions = scan_chain->getScanPartitions();
    int chain_suffix = 0;
    for (dbScanPartition* scan_partition : scan_partitions) {
      bool already_printed_floating = false;
      bool already_printed_ordered = false;
      const std::string chain_name
          = scan_partitions.size() == 1
                ? scan_chain->getName()
                : fmt::format("{}_{}", scan_chain->getName(), chain_suffix);

      const std::string start_pin_name = std::visit(
          [](auto&& pin) { return pin->getName(); }, scan_chain->getScanIn());
      const std::string stop_pin_name = std::visit(
          [](auto&& pin) { return pin->getName(); }, scan_chain->getScanOut());

      fprintf(_out, "- %s\n", chain_name.c_str());
      fprintf(_out, "+ START PIN %s\n", start_pin_name.c_str());

      for (dbScanList* scan_list : scan_partition->getScanLists()) {
        dbSet<dbScanInst> scan_insts = scan_list->getScanInsts();
        if (scan_insts.size() == 1 && !already_printed_floating) {
          fprintf(_out, "+ FLOATING\n");
          already_printed_floating = true;
          already_printed_ordered = false;
        } else if (scan_insts.size() > 1 && !already_printed_ordered) {
          fprintf(_out, "+ ORDERED\n");
          already_printed_floating = false;
          already_printed_ordered = true;
        }

        for (dbScanInst* scan_inst : scan_insts) {
          dbScanInst::AccessPins access_pins = scan_inst->getAccessPins();
          const std::string scan_in_name = std::visit(
              [](auto&& pin) { return getPinName(pin); }, access_pins.scan_in);
          const std::string scan_out_name = std::visit(
              [](auto&& pin) { return getPinName(pin); }, access_pins.scan_out);
          fprintf(_out,
                  "  %s ( IN %s ) ( OUT %s )\n",
                  scan_inst->getInst()->getName().c_str(),
                  scan_in_name.c_str(),
                  scan_out_name.c_str());
        }
      }
      fprintf(_out, "+ PARTITION %s\n", scan_partition->getName().c_str());
      fprintf(_out, "+ STOP PIN %s ;\n\n", stop_pin_name.c_str());
      ++chain_suffix;
    }
  }

  fprintf(_out, "END SCANCHAINS\n\n");
}

void defout_impl::writeBTerm(dbBTerm* bterm)
{
  dbNet* net = bterm->getNet();
  if (net) {
    dbSet<dbBPin> bpins = bterm->getBPins();

    if (bpins.size() != 0) {
      int cnt = 0;

      dbSet<dbBPin>::iterator itr;

      for (itr = bpins.begin(); itr != bpins.end(); ++itr) {
        writeBPin(*itr, cnt++);
      }

      fprintf(_out, " ;\n");

      return;
    }

    std::string bname = bterm->getName();

    if (_use_net_inst_ids) {
      fprintf(_out, "    - %s + NET N%u", bname.c_str(), net->getId());
    } else {
      std::string nname = net->getName();
      fprintf(_out, "    - %s + NET %s", bname.c_str(), nname.c_str());
    }

    if (bterm->isSpecial()) {
      fprintf(_out, " + SPECIAL");
    }

    fprintf(_out, " + DIRECTION %s", defIoType(bterm->getIoType()));

    if (_version >= defout::DEF_5_6) {
      dbBTerm* supply = bterm->getSupplyPin();

      if (supply) {
        std::string pname = supply->getName();
        fprintf(_out, " + SUPPLYSENSITIVITY %s", pname.c_str());
      }

      dbBTerm* ground = bterm->getGroundPin();

      if (ground) {
        std::string pname = ground->getName();
        fprintf(_out, " + GROUNDSENSITIVITY %s", pname.c_str());
      }
    }

    const char* sig_type = defSigType(bterm->getSigType());
    fprintf(_out, " + USE %s", sig_type);

    fprintf(_out, " ;\n");
  } else {
    _logger->warn(utl::ODB,
                  173,
                  "warning: pin {} skipped because it has no net",
                  bterm->getConstName());
  }
}


void defout_impl::writeBTerm_Pl(dbBTerm* bterm)
{
  dbNet* net = bterm->getNet();
  if (net) {
    dbSet<dbBPin> bpins = bterm->getBPins();

    if (bpins.size() != 0) {
      int cnt = 0;

      dbSet<dbBPin>::iterator itr;

      for (itr = bpins.begin(); itr != bpins.end(); ++itr) {
        writeBPin_Pl(*itr, cnt++);
      }
    }

    std::string bname = bterm->getName();
    fprintf(_out, "\n");
  } 
}



void defout_impl::writeBPin(dbBPin* bpin, int cnt)
{
  dbBTerm* bterm = bpin->getBTerm();
  dbNet* net = bterm->getNet();
  std::string bname = bterm->getName();

  if (cnt == 0 || _version <= defout::DEF_5_6) {
    if (_use_net_inst_ids) {
      if (cnt == 0) {
        fprintf(_out, "    - %s + NET N%u", bname.c_str(), net->getId());
      } else {
        fprintf(_out,
                "    - %s.extra%d + NET N%u",
                bname.c_str(),
                cnt,
                net->getId());
      }
    } else {
      std::string nname = net->getName();
      if (cnt == 0) {
        fprintf(_out, "    - %s + NET %s", bname.c_str(), nname.c_str());
      } else {
        fprintf(_out,
                "    - %s.extra%d + NET %s",
                bname.c_str(),
                cnt,
                nname.c_str());
      }
    }

    if (bterm->isSpecial()) {
      fprintf(_out, " + SPECIAL");
    }

    fprintf(_out, " + DIRECTION %s", defIoType(bterm->getIoType()));

    if (_version >= defout::DEF_5_6) {
      dbBTerm* supply = bterm->getSupplyPin();

      if (supply) {
        std::string pname = supply->getName();
        fprintf(_out, " + SUPPLYSENSITIVITY %s", pname.c_str());
      }

      dbBTerm* ground = bterm->getGroundPin();

      if (ground) {
        std::string pname = ground->getName();
        fprintf(_out, " + GROUNDSENSITIVITY %s", pname.c_str());
      }
    }

    fprintf(_out, " + USE %s", defSigType(bterm->getSigType()));
  }

  fprintf(_out, "\n      ");

  if (_version > defout::DEF_5_6) {
    fprintf(_out, "+ PORT");
  }

  bool isFirst = true;
  int dw, dh, x = 0, y = 0;
  int xMin, yMin, xMax, yMax;

  for (dbBox* box : bpin->getBoxes()) {
    dw = defdist(int(box->getDX() / 2));
    dh = defdist(int(box->getDY() / 2));

    if (isFirst) {
      isFirst = false;
      x = defdist(box->xMin()) + dw;
      y = defdist(box->yMin()) + dh;
    }

    xMin = defdist(box->xMin()) - x;
    yMin = defdist(box->yMin()) - y;
    xMax = defdist(box->xMax()) - x;
    yMax = defdist(box->yMax()) - y;
    dbTechLayer* layer = box->getTechLayer();
    std::string lname;

    if (_use_alias && layer->hasAlias()) {
      lname = layer->getAlias();
    } else {
      lname = layer->getName();
    }

    fprintf(_out, "\n       ");
    if (_version == defout::DEF_5_5) {
      fprintf(_out,
              " + LAYER %s ( %d %d ) ( %d %d )",
              lname.c_str(),
              xMin,
              yMin,
              xMax,
              yMax);
    } else {
      std::string layer_name = lname;
      if (_version == defout::DEF_5_8) {
        uint mask = box->getLayerMask();
        if (mask != 0) {
          // add mask information to layer name
          layer_name += " MASK " + std::to_string(mask);
        }
      }
      if (bpin->hasEffectiveWidth()) {
        int w = defdist(bpin->getEffectiveWidth());
        fprintf(_out,
                " + LAYER %s DESIGNRULEWIDTH %d ( %d %d ) ( %d %d )",
                layer_name.c_str(),
                w,
                xMin,
                yMin,
                xMax,
                yMax);
      } else if (bpin->hasMinSpacing()) {
        int s = defdist(bpin->getMinSpacing());
        fprintf(_out,
                " + LAYER %s SPACING %d ( %d %d ) ( %d %d )",
                layer_name.c_str(),
                s,
                xMin,
                yMin,
                xMax,
                yMax);
      } else {
        fprintf(_out,
                " + LAYER %s ( %d %d ) ( %d %d )",
                layer_name.c_str(),
                xMin,
                yMin,
                xMax,
                yMax);
      }
    }
  }

  dbPlacementStatus status = bpin->getPlacementStatus();

  switch (status.getValue()) {
    case dbPlacementStatus::NONE:
    case dbPlacementStatus::UNPLACED:
      break;

    case dbPlacementStatus::SUGGESTED:
    case dbPlacementStatus::PLACED: {
      fprintf(_out, "\n        + PLACED ( %d %d ) N", x, y);
      break;
    }

    case dbPlacementStatus::LOCKED:
    case dbPlacementStatus::FIRM: {
      fprintf(_out, "\n        + FIXED ( %d %d ) N", x, y);
      break;
    }

    case dbPlacementStatus::COVER: {
      fprintf(_out, "\n        + COVER ( %d %d ) N", x, y);
      break;
    }
  }
}

void defout_impl::writeBPin_Pl(dbBPin* bpin, int cnt)
{
  if (cnt != 0) {
    return;
  }


  dbBTerm* bterm = bpin->getBTerm();
  std::string bname = bterm->getName();

  fprintf(_out, "%s", bname.c_str());

  bool isFirst = true;
  int dw, dh, x = 0, y = 0;

  for (dbBox* box : bpin->getBoxes()) {
    dw = defdist(int(box->getDX() / 2));
    dh = defdist(int(box->getDY() / 2));

    if (isFirst) {
      isFirst = false;
      x = defdist(box->xMin()) + dw;
      y = defdist(box->yMin()) + dh;
    }
  }

  dbPlacementStatus status = bpin->getPlacementStatus();

  switch (status.getValue()) {
    case dbPlacementStatus::NONE:
    case dbPlacementStatus::UNPLACED:
      break;

    case dbPlacementStatus::SUGGESTED:
    case dbPlacementStatus::PLACED: {
      fprintf(_out, " %d %d : N", x, y);
      break;
    }

    case dbPlacementStatus::LOCKED:
    case dbPlacementStatus::FIRM: {
      fprintf(_out, " %d %d : N", x, y);
      break;
    }

    case dbPlacementStatus::COVER: {
      fprintf(_out, " %d %d : N", x, y);
      break;  
    }
  }
}




void defout_impl::writeBlockages(dbBlock* block)
{
  dbSet<dbObstruction> obstructions_raw = block->getObstructions();
  dbSet<dbBlockage> blockages_raw = block->getBlockages();

  std::vector<dbObstruction*> obstructions;
  std::vector<dbBlockage*> blockages;

  for (const auto& obstruction : obstructions_raw) {
    if (obstruction->isSystemReserved()) {
      continue;
    }
    obstructions.push_back(obstruction);
  }

  for (const auto& blockage : blockages_raw) {
    if (blockage->isSystemReserved()) {
      continue;
    }
    blockages.push_back(blockage);
  }

  int bcnt = obstructions.size() + blockages.size();

  if (bcnt == 0) {
    return;
  }

  bool first = true;

  std::vector<dbObstruction*> sorted_obs(obstructions.begin(),
                                         obstructions.end());
  std::sort(sorted_obs.begin(),
            sorted_obs.end(),
            [](dbObstruction* a, dbObstruction* b) {
              dbBox* bbox_a = a->getBBox();
              dbTechLayer* layer_a = bbox_a->getTechLayer();

              dbBox* bbox_b = b->getBBox();
              dbTechLayer* layer_b = bbox_a->getTechLayer();
              if (layer_a != layer_b) {
                return layer_a->getNumber() < layer_b->getNumber();
              }

              Rect rect_a = bbox_a->getBox();
              Rect rect_b = bbox_b->getBox();
              return rect_a < rect_b;
            });
  for (dbObstruction* obs : sorted_obs) {
    dbInst* inst = obs->getInstance();
    if (inst && _select_inst_map && !(*_select_inst_map)[inst]) {
      continue;
    }

    if (first) {
      first = false;
      fprintf(_out, "BLOCKAGES %d ;\n", bcnt);
    }

    dbBox* bbox = obs->getBBox();
    dbTechLayer* layer = bbox->getTechLayer();
    std::string lname;
    if (_use_alias && layer->hasAlias()) {
      lname = layer->getAlias();
    } else {
      lname = layer->getName();
    }

    fprintf(_out, "    - LAYER %s", lname.c_str());

    if (inst) {
      if (_use_net_inst_ids) {
        fprintf(_out, " + COMPONENT I%u", inst->getId());
      } else {
        std::string iname = inst->getName();
        fprintf(_out, " + COMPONENT %s", iname.c_str());
      }
    }

    if (obs->isSlotObstruction()) {
      fprintf(_out, " + SLOTS");
    }

    if (obs->isFillObstruction()) {
      fprintf(_out, " + FILLS");
    }

    if (obs->isPushedDown()) {
      fprintf(_out, " + PUSHDOWN");
    }

    if (_version >= defout::DEF_5_6) {
      if (obs->hasEffectiveWidth()) {
        int w = defdist(obs->getEffectiveWidth());
        fprintf(_out, " + DESIGNRULEWIDTH %d", w);
      } else if (obs->hasMinSpacing()) {
        int s = defdist(obs->getMinSpacing());
        fprintf(_out, " + SPACING %d", s);
      }
    }

    int x1 = defdist(bbox->xMin());
    int y1 = defdist(bbox->yMin());
    int x2 = defdist(bbox->xMax());
    int y2 = defdist(bbox->yMax());

    fprintf(_out, " RECT ( %d %d ) ( %d %d ) ;\n", x1, y1, x2, y2);
  }

  std::vector<dbBlockage*> sorted_blockages(blockages.begin(), blockages.end());
  std::sort(sorted_blockages.begin(),
            sorted_blockages.end(),
            [](dbBlockage* a, dbBlockage* b) {
              dbBox* bbox_a = a->getBBox();
              dbBox* bbox_b = b->getBBox();
              Rect rect_a = bbox_a->getBox();
              Rect rect_b = bbox_b->getBox();
              return rect_a < rect_b;
            });

  for (dbBlockage* blk : sorted_blockages) {
    dbInst* inst = blk->getInstance();
    if (inst && _select_inst_map && !(*_select_inst_map)[inst]) {
      continue;
    }

    if (first) {
      first = false;
      fprintf(_out, "BLOCKAGES %d ;\n", bcnt);
    }

    fprintf(_out, "    - PLACEMENT");

    if (blk->isSoft()) {
      fprintf(_out, " + SOFT");
    }

    if (blk->getMaxDensity() > 0) {
      fprintf(_out, " + PARTIAL %f", blk->getMaxDensity());
    }

    if (inst) {
      if (_use_net_inst_ids) {
        fprintf(_out, " + COMPONENT I%u", inst->getId());
      } else {
        std::string iname = inst->getName();
        fprintf(_out, " + COMPONENT %s", iname.c_str());
      }
    }

    if (blk->isPushedDown()) {
      fprintf(_out, " + PUSHDOWN");
    }

    dbBox* bbox = blk->getBBox();
    int x1 = defdist(bbox->xMin());
    int y1 = defdist(bbox->yMin());
    int x2 = defdist(bbox->xMax());
    int y2 = defdist(bbox->yMax());

    fprintf(_out, " RECT ( %d %d ) ( %d %d ) ;\n", x1, y1, x2, y2);
  }

  if (!first) {
    fprintf(_out, "END BLOCKAGES\n");
  }
}

void defout_impl::writeFills(dbBlock* block)
{
  dbSet<dbFill> fills = block->getFills();
  int num_fills = fills.size();

  if (num_fills == 0) {
    return;
  }

  fprintf(_out, "FILLS %d ;\n", num_fills);

  for (dbFill* fill : fills) {
    fprintf(_out, "    - LAYER %s", fill->getTechLayer()->getName().c_str());

    uint mask = fill->maskNumber();
    if (mask != 0) {
      fprintf(_out, " + MASK %u", mask);
    }

    if (fill->needsOPC()) {
      fprintf(_out, " + OPC");
    }

    Rect r;
    fill->getRect(r);

    int x1 = defdist(r.xMin());
    int y1 = defdist(r.yMin());
    int x2 = defdist(r.xMax());
    int y2 = defdist(r.yMax());

    fprintf(_out, " RECT ( %d %d ) ( %d %d ) ;\n", x1, y1, x2, y2);
  }

  fprintf(_out, "END FILLS\n");
}

void defout_impl::writeNets(dbBlock* block)
{
  dbSet<dbNet> nets = block->getNets();

  int net_cnt = 0;
  int snet_cnt = 0;

  dbSet<dbNet>::iterator itr;
  dbMap<dbNet, char> regular_net(nets);

  auto sorted_nets = sortedSet(nets);

  for (dbNet* net : sorted_nets) {
    if (_select_net_map) {
      if (!(*_select_net_map)[net]) {
        continue;
      }
    }

    if (!net->isSpecial()) {
      regular_net[net] = 1;
      net_cnt++;
    } else {
      regular_net[net] = 0;
      snet_cnt++;

      // Check for non-special iterms.
      for (dbITerm* iterm : net->getITerms()) {
        if (!iterm->isSpecial()) {
          regular_net[net] = 1;
          net_cnt++;
          break;
        }
      }
    }
  }

  if (snet_cnt > 0) {
    fprintf(_out, "SPECIALNETS %d ;\n", snet_cnt);

    for (dbNet* net : sorted_nets) {
      if (_select_net_map && !(*_select_net_map)[net]) {
        continue;
      }
      if (net->isSpecial()) {
        writeSNet(net);
      }
    }

    fprintf(_out, "END SPECIALNETS\n");
  }

  fprintf(_out, "NETS %d ;\n", net_cnt);

  for (dbNet* net : sorted_nets) {
    if (_select_net_map && !(*_select_net_map)[net]) {
      continue;
    }

    if (regular_net[net] == 1) {
      writeNet(net);
    }
  }

  fprintf(_out, "END NETS\n");
}

void defout_impl::writeSNet(dbNet* net)
{
  dbSet<dbITerm> iterms = net->getITerms();

  if (_use_net_inst_ids) {
    fprintf(_out, "    - N%u", net->getId());
  } else {
    std::string nname = net->getName();
    fprintf(_out, "    - %s", nname.c_str());
  }

  int i = 0;

  for (dbBTerm* bterm : net->getBTerms()) {
    if ((++i & 7) == 0) {
      fprintf(_out, "\n    ");
    }
    fprintf(_out, " ( PIN %s )", bterm->getName().c_str());
  }

  char ttname[max_name_length];
  dbSet<dbITerm>::iterator iterm_itr;
  std::set<std::string> wild_names;
  for (iterm_itr = iterms.begin(); iterm_itr != iterms.end(); ++iterm_itr) {
    dbITerm* iterm = *iterm_itr;

    if (!iterm->isSpecial()) {
      continue;
    }

    dbInst* inst = iterm->getInst();
    dbMTerm* mterm = iterm->getMTerm();
    char* mtname = mterm->getName(inst, &ttname[0]);
    if (net->isWildConnected()) {
      if (wild_names.find(mtname) == wild_names.end()) {
        fprintf(_out, " ( * %s )", mtname);
        ++i;
        wild_names.insert(mtname);
      }
    } else {
      if ((++i & 7) == 0) {
        if (_use_net_inst_ids) {
          fprintf(_out, "\n      ( I%u %s )", inst->getId(), mtname);
        } else {
          std::string iname = inst->getName();
          fprintf(_out, "\n      ( %s %s )", iname.c_str(), mtname);
        }
      } else {
        if (_use_net_inst_ids) {
          fprintf(_out, " ( I%u %s )", inst->getId(), mtname);
        } else {
          std::string iname = inst->getName();
          fprintf(_out, " ( %s %s )", iname.c_str(), mtname);
        }
      }
    }
  }

  const char* sig_type = defSigType(net->getSigType());
  fprintf(_out, " + USE %s", sig_type);

  _non_default_rule = nullptr;
  dbSet<dbSWire> swires = net->getSWires();
  dbSet<dbSWire>::iterator itr;

  for (itr = swires.begin(); itr != swires.end(); ++itr) {
    writeSWire(*itr);
  }

  dbSourceType source = net->getSourceType();

  switch (source.getValue()) {
    case dbSourceType::NONE:
      break;

    case dbSourceType::NETLIST:
      fprintf(_out, " + SOURCE NETLIST");
      break;

    case dbSourceType::DIST:
      fprintf(_out, " + SOURCE DIST");
      break;

    case dbSourceType::USER:
      fprintf(_out, " + SOURCE USER");
      break;

    case dbSourceType::TIMING:
      fprintf(_out, " + SOURCE TIMING");
      break;

    case dbSourceType::TEST:
      break;
  }

  if (net->hasFixedBump()) {
    fprintf(_out, " + FIXEDBUMP");
  }

  if (net->getWeight() != 1) {
    fprintf(_out, " + WEIGHT %d", net->getWeight());
  }

  if (hasProperties(net, SPECIALNET)) {
    fprintf(_out, " + PROPERTY ");
    writeProperties(net);
  }

  fprintf(_out, " ;\n");
}

void defout_impl::writeWire(dbWire* wire)
{
  dbWireDecoder decode;
  dbTechLayer* layer;
  dbWireType prev_wire_type = dbWireType::NONE;
  int point_cnt = 0;
  int path_cnt = 0;
  int prev_x = std::numeric_limits<int>::max();
  int prev_y = std::numeric_limits<int>::max();

  for (decode.begin(wire);;) {
    dbWireDecoder::OpCode opcode = decode.next();
    std::optional<uint8_t> color = decode.getColor();
    std::optional<dbWireDecoder::ViaColor> viacolor = decode.getViaColor();

    switch (opcode) {
      case dbWireDecoder::PATH:
      case dbWireDecoder::SHORT:
      case dbWireDecoder::VWIRE:
      case dbWireDecoder::JUNCTION: {
        layer = decode.getLayer();
        std::string lname;
        if (_use_alias && layer->hasAlias()) {
          lname = layer->getAlias();
        } else {
          lname = layer->getName();
        }
        dbWireType wire_type = decode.getWireType();
        if (wire->getNet()->getWireType() == dbWireType::FIXED) {
          wire_type = dbWireType::FIXED;
        }

        if ((path_cnt == 0) || (wire_type != prev_wire_type)) {
          fprintf(
              _out, "\n      + %s %s", wire_type.getString(), lname.c_str());
        } else {
          fprintf(_out, "\n      NEW %s", lname.c_str());
        }

        if (_non_default_rule && (decode.peek() != dbWireDecoder::RULE)) {
          fprintf(_out, " TAPER");
        }

        prev_wire_type = wire_type;
        point_cnt = 0;
        ++path_cnt;
        break;
      }

      case dbWireDecoder::POINT: {
        int x, y;
        decode.getPoint(x, y);
        x = defdist(x);
        y = defdist(y);

        if ((++point_cnt & 7) == 0) {
          fprintf(_out, "\n    ");
        }

        std::string mask_statement;
        if (point_cnt % 2 == 0 && color) {
          mask_statement = fmt::format("MASK {}", color.value());
        }

        if (point_cnt == 1) {
          fprintf(_out, " ( %d %d )", x, y);
        } else if (x == prev_x) {
          fprintf(_out, "%s ( * %d )", mask_statement.c_str(), y);
        } else if (y == prev_y) {
          fprintf(_out, "%s ( %d * )", mask_statement.c_str(), x);
        }

        prev_x = x;
        prev_y = y;
        break;
      }

      case dbWireDecoder::POINT_EXT: {
        int x, y, ext;
        decode.getPoint(x, y, ext);
        x = defdist(x);
        y = defdist(y);
        ext = defdist(ext);

        if ((++point_cnt & 7) == 0) {
          fprintf(_out, "\n    ");
        }

        if (point_cnt == 1) {
          fprintf(_out, " ( %d %d %d )", x, y, ext);
        } else if ((x == prev_x) && (y == prev_y)) {
          fprintf(_out, " ( * * %d )", ext);
        } else if (x == prev_x) {
          fprintf(_out, " ( * %d %d )", y, ext);
        } else if (y == prev_y) {
          fprintf(_out, " ( %d * %d )", x, ext);
        }

        prev_x = x;
        prev_y = y;
        break;
      }

      case dbWireDecoder::VIA: {
        if ((++point_cnt & 7) == 0) {
          fprintf(_out, "\n    ");
        }

        dbVia* via = decode.getVia();

        std::string via_mask_statement;
        if ((_version >= defout::DEF_5_8) && viacolor) {
          via_mask_statement = fmt::format("MASK {}{}{} ",
                                           viacolor.value().top_color,
                                           viacolor.value().cut_color,
                                           viacolor.value().bottom_color);
        }

        if ((_version >= defout::DEF_5_6) && via->isViaRotated()) {
          std::string vname;

          if (via->getTechVia()) {
            vname = via->getTechVia()->getName();
          } else {
            vname = via->getBlockVia()->getName();
          }

          fprintf(_out,
                  " %s%s %s",
                  via_mask_statement.c_str(),
                  vname.c_str(),
                  defOrient(via->getOrient()));
        } else {
          std::string vname = via->getName();
          fprintf(_out, " %s%s", via_mask_statement.c_str(), vname.c_str());
        }
        break;
      }

      case dbWireDecoder::TECH_VIA: {
        if ((++point_cnt & 7) == 0) {
          fprintf(_out, "\n    ");
        }

        std::string via_mask_statement;
        if ((_version >= defout::DEF_5_8) && viacolor) {
          via_mask_statement = fmt::format("MASK {}{}{} ",
                                           viacolor.value().top_color,
                                           viacolor.value().cut_color,
                                           viacolor.value().bottom_color);
        }

        dbTechVia* via = decode.getTechVia();
        std::string vname = via->getName();
        fprintf(_out, " %s%s", via_mask_statement.c_str(), vname.c_str());
        break;
      }

      case dbWireDecoder::ITERM:
      case dbWireDecoder::BTERM:
        break;

      case dbWireDecoder::RULE: {
        if (point_cnt == 0) {
          dbTechLayerRule* rule = decode.getRule();
          dbTechNonDefaultRule* taper_rule = rule->getNonDefaultRule();

          if (_non_default_rule == nullptr) {
            std::string name = taper_rule->getName();
            fprintf(_out, " TAPERRULE %s ", name.c_str());
          } else if (_non_default_rule != taper_rule) {
            std::string name = taper_rule->getName();
            fprintf(_out, " TAPERRULE %s ", name.c_str());
          }
        }
        break;
      }

      case dbWireDecoder::RECT: {
        if ((++point_cnt & 7) == 0) {
          fprintf(_out, "\n    ");
        }

        int deltaX1;
        int deltaY1;
        int deltaX2;
        int deltaY2;
        decode.getRect(deltaX1, deltaY1, deltaX2, deltaY2);
        deltaX1 = defdist(deltaX1);
        deltaY1 = defdist(deltaY1);
        deltaX2 = defdist(deltaX2);
        deltaY2 = defdist(deltaY2);
        if (color.has_value()) {
          fprintf(_out,
                  " RECT MASK %d ( %d %d %d %d ) ",
                  color.value(),
                  deltaX1,
                  deltaY1,
                  deltaX2,
                  deltaY2);

        } else {
          fprintf(_out,
                  " RECT ( %d %d %d %d ) ",
                  deltaX1,
                  deltaY1,
                  deltaX2,
                  deltaY2);
        }
        break;
      }

      case dbWireDecoder::END_DECODE:
        return;
    }
  }
}

void defout_impl::writeSWire(dbSWire* wire)
{
  switch (wire->getWireType().getValue()) {
    case dbWireType::COVER:
      fprintf(_out, "\n      + COVER");
      break;

    case dbWireType::FIXED:
      fprintf(_out, "\n      + FIXED");
      break;

    case dbWireType::ROUTED:
      fprintf(_out, "\n      + ROUTED");
      break;

    case dbWireType::SHIELD: {
      dbNet* s = wire->getShield();
      if (s) {
        std::string n = s->getName();
        fprintf(_out, "\n      + SHIELD %s", n.c_str());
      } else {
        _logger->warn(utl::ODB, 174, "warning: missing shield net");
        fprintf(_out, "\n      + ROUTED");
      }
      break;
    }

    default:
      fprintf(_out, "\n      + ROUTED");
      break;
  }

  int i = 0;
  dbSet<dbSBox> wires = wire->getWires();
  dbSet<dbSBox>::iterator itr;

  for (itr = wires.begin(); itr != wires.end(); ++itr) {
    dbSBox* box = *itr;

    if (i++ > 0) {
      fprintf(_out, "\n      NEW");
    }

    if (!box->isVia()) {
      writeSpecialPath(box);

    } else if (box->getTechVia()) {
      dbWireShapeType type = box->getWireShapeType();
      dbTechVia* v = box->getTechVia();
      std::string vn = v->getName();
      dbTechLayer* l = v->getBottomLayer();
      std::string ln;
      if (_use_alias && l->hasAlias()) {
        ln = l->getAlias();
      } else {
        ln = l->getName();
      }

      int x, y;
      box->getViaXY(x, y);

      if (box->hasViaLayerMasks()) {
        vn = fmt::format("MASK {}{}{} {}",
                         box->getViaTopLayerMask(),
                         box->getViaCutLayerMask(),
                         box->getViaBottomLayerMask(),
                         vn);
      }

      if (type.getValue() == dbWireShapeType::NONE) {
        fprintf(_out,
                " %s 0 ( %d %d ) %s",
                ln.c_str(),
                defdist(x),
                defdist(y),
                vn.c_str());
      } else {
        fprintf(_out,
                " %s 0 + SHAPE %s ( %d %d ) %s",
                ln.c_str(),
                type.getString(),
                defdist(x),
                defdist(y),
                vn.c_str());
      }
    } else if (box->getBlockVia()) {
      dbWireShapeType type = box->getWireShapeType();
      dbVia* v = box->getBlockVia();
      std::string vn = v->getName();
      dbTechLayer* l = v->getBottomLayer();
      std::string ln;
      if (_use_alias && l->hasAlias()) {
        ln = l->getAlias();
      } else {
        ln = l->getName();
      }

      int x, y;
      box->getViaXY(x, y);

      if (box->hasViaLayerMasks()) {
        vn = fmt::format("MASK {}{}{} {}",
                         box->getViaTopLayerMask(),
                         box->getViaCutLayerMask(),
                         box->getViaBottomLayerMask(),
                         vn);
      }

      if (type.getValue() == dbWireShapeType::NONE) {
        fprintf(_out,
                " %s 0 ( %d %d ) %s",
                ln.c_str(),
                defdist(x),
                defdist(y),
                vn.c_str());
      } else {
        fprintf(_out,
                " %s 0 + SHAPE %s ( %d %d ) %s",
                ln.c_str(),
                type.getString(),
                defdist(x),
                defdist(y),
                vn.c_str());
      }
    }
  }
}

void defout_impl::writeSpecialPath(dbSBox* box)
{
  dbTechLayer* l = box->getTechLayer();
  std::string ln;

  if (_use_alias && l->hasAlias()) {
    ln = l->getAlias();
  } else {
    ln = l->getName();
  }

  int x1 = box->xMin();
  int y1 = box->yMin();
  int x2 = box->xMax();
  int y2 = box->yMax();
  uint dx = x2 - x1;
  uint dy = y2 - y1;
  uint w;
  uint mask = box->getLayerMask();

  switch (box->getDirection()) {
    case dbSBox::UNDEFINED: {
      bool dx_even = ((dx & 1) == 0);
      bool dy_even = ((dy & 1) == 0);

      if (dx_even && dy_even) {
        if (dy < dx) {
          w = dy;
          uint dw = dy >> 1;
          y1 += dw;
          y2 -= dw;
          assert(y1 == y2);
        } else {
          w = dx;
          uint dw = dx >> 1;
          x1 += dw;
          x2 -= dw;
          assert(x1 == x2);
        }
      } else if (dx_even) {
        w = dx;
        uint dw = dx >> 1;
        x1 += dw;
        x2 -= dw;
        assert(x1 == x2);
      } else if (dy_even) {
        w = dy;
        uint dw = dy >> 1;
        y1 += dw;
        y2 -= dw;
        assert(y1 == y2);
      } else {
        throw ZException("odd dimension in both directions");
      }

      break;
    }

    case dbSBox::HORIZONTAL: {
      w = dy;
      uint dw = dy >> 1;
      y1 += dw;
      y2 -= dw;
      assert(y1 == y2);
      break;
    }

    case dbSBox::VERTICAL: {
      w = dx;
      uint dw = dx >> 1;
      x1 += dw;
      x2 -= dw;
      assert(x1 == x2);
      break;
    }
    case dbSBox::OCTILINEAR: {
      Oct oct = box->getOct();
      x1 = oct.getCenterLow().getX();
      y1 = oct.getCenterLow().getY();
      x2 = oct.getCenterHigh().getX();
      y2 = oct.getCenterHigh().getY();
      w = oct.getWidth();
      break;
    }
    default:
      throw ZException("unknown direction");
      break;
  }

  dbWireShapeType type = box->getWireShapeType();

  if (mask != 0) {
    if (type.getValue() == dbWireShapeType::NONE) {
      fprintf(_out,
              " %s %d ( %d %d ) MASK %d ( %d %d )",
              ln.c_str(),
              defdist(w),
              defdist(x1),
              defdist(y1),
              mask,
              defdist(x2),
              defdist(y2));
    } else {
      fprintf(_out,
              " %s %d + SHAPE %s + MASK %d + ( %d %d ) ( %d %d )",
              ln.c_str(),
              defdist(w),
              type.getString(),
              mask,
              defdist(x1),
              defdist(y1),
              defdist(x2),
              defdist(y2));
    }
  } else {
    if (type.getValue() == dbWireShapeType::NONE) {
      fprintf(_out,
              " %s %d ( %d %d ) ( %d %d )",
              ln.c_str(),
              defdist(w),
              defdist(x1),
              defdist(y1),
              defdist(x2),
              defdist(y2));
    } else {
      fprintf(_out,
              " %s %d + SHAPE %s ( %d %d ) ( %d %d )",
              ln.c_str(),
              defdist(w),
              type.getString(),
              defdist(x1),
              defdist(y1),
              defdist(x2),
              defdist(y2));
    }
  }
}

void defout_impl::writeNet(dbNet* net)
{
  if (_use_net_inst_ids) {
    fprintf(_out, "    - N%u", net->getId());
  } else {
    std::string nname = net->getName();
    fprintf(_out, "    - %s", nname.c_str());
  }

  char ttname[max_name_length];
  int i = 0;

  for (dbBTerm* bterm : net->getBTerms()) {
    const char* pin_name = bterm->getConstName();
    if ((++i & 7) == 0) {
      fprintf(_out, "\n     ");
    }
    fprintf(_out, " ( PIN %s )", pin_name);
  }

  for (dbITerm* iterm : net->getITerms()) {
    if (iterm->isSpecial()) {
      continue;
    }

    dbInst* inst = iterm->getInst();
    if (_select_inst_map && !(*_select_inst_map)[inst]) {
      continue;  // for power nets in regular net section, tie-lo/hi
    }
    dbMTerm* mterm = iterm->getMTerm();
    // std::string mtname = mterm->getName();
    char* mtname = mterm->getName(inst, &ttname[0]);

    if ((++i & 7) == 0) {
      fprintf(_out, "\n     ");
    }

    if (_use_net_inst_ids) {
      fprintf(_out, " ( I%u %s )", inst->getId(), mtname);
    } else {
      std::string iname = inst->getName();
      fprintf(_out, " ( %s %s )", iname.c_str(), mtname);
    }
  }

  if (net->getXTalkClass() != 0) {
    fprintf(_out, " + XTALK %d", net->getXTalkClass());
  }

  const char* sig_type = defSigType(net->getSigType());
  fprintf(_out, " + USE %s", sig_type);

  _non_default_rule = net->getNonDefaultRule();

  if (_non_default_rule) {
    std::string n = _non_default_rule->getName();
    fprintf(_out, " + NONDEFAULTRULE %s", n.c_str());
  }

  dbWire* wire = net->getWire();

  if (wire) {
    writeWire(wire);
  }

  dbSourceType source = net->getSourceType();

  switch (source.getValue()) {
    case dbSourceType::NONE:
      break;

    case dbSourceType::NETLIST:
      fprintf(_out, " + SOURCE NETLIST");
      break;

    case dbSourceType::DIST:
      fprintf(_out, " + SOURCE DIST");
      break;

    case dbSourceType::USER:
      fprintf(_out, " + SOURCE USER");
      break;

    case dbSourceType::TIMING:
      fprintf(_out, " + SOURCE TIMING");
      break;

    case dbSourceType::TEST:
      fprintf(_out, " + SOURCE TEST");
      break;
  }

  if (net->hasFixedBump()) {
    fprintf(_out, " + FIXEDBUMP");
  }

  if (net->getWeight() != 1) {
    fprintf(_out, " + WEIGHT %d", net->getWeight());
  }

  if (hasProperties(net, NET)) {
    fprintf(_out, " + PROPERTY ");
    writeProperties(net);
  }

  fprintf(_out, " ;\n");
}

//
// See defin/definProDefs.h
//
void defout_impl::writePropertyDefinitions(dbBlock* block)
{
  dbProperty* defs
      = dbProperty::find(block, "__ADS_DEF_PROPERTY_DEFINITIONS__");

  if (defs == nullptr) {
    return;
  }

  fprintf(_out, "PROPERTYDEFINITIONS\n");

  dbSet<dbProperty> obj_types = dbProperty::getProperties(defs);
  dbSet<dbProperty>::iterator objitr;

  for (objitr = obj_types.begin(); objitr != obj_types.end(); ++objitr) {
    dbProperty* obj = *objitr;
    std::string objType = obj->getName();

    ObjType obj_type;

    if (strcmp(objType.c_str(), "COMPONENT") == 0) {
      obj_type = COMPONENT;
    } else if (strcmp(objType.c_str(), "COMPONENTPIN") == 0) {
      obj_type = COMPONENTPIN;
    } else if (strcmp(objType.c_str(), "DESIGN") == 0) {
      obj_type = DESIGN;
    } else if (strcmp(objType.c_str(), "GROUP") == 0) {
      obj_type = GROUP;
    } else if (strcmp(objType.c_str(), "NET") == 0) {
      obj_type = NET;
    } else if (strcmp(objType.c_str(), "NONDEFAULTRULE") == 0) {
      obj_type = NONDEFAULTRULE;
    } else if (strcmp(objType.c_str(), "REGION") == 0) {
      obj_type = REGION;
    } else if (strcmp(objType.c_str(), "ROW") == 0) {
      obj_type = ROW;
    } else if (strcmp(objType.c_str(), "SPECIALNET") == 0) {
      obj_type = SPECIALNET;
    } else {
      continue;
    }

    std::map<std::string, bool>& defs_map = _prop_defs[obj_type];
    dbSet<dbProperty> props = dbProperty::getProperties(obj);
    dbSet<dbProperty>::iterator pitr;

    for (pitr = props.begin(); pitr != props.end(); ++pitr) {
      dbProperty* prop = *pitr;
      std::string name = prop->getName();
      defs_map[std::string(name)] = true;
      switch (prop->getType()) {
        case dbProperty::STRING_PROP:
          fprintf(_out, "%s %s STRING ", objType.c_str(), name.c_str());
          break;

        case dbProperty::INT_PROP:
          fprintf(_out, "%s %s INTEGER ", objType.c_str(), name.c_str());
          break;

        case dbProperty::DOUBLE_PROP:
          fprintf(_out, "%s %s REAL ", objType.c_str(), name.c_str());
          break;

        default:
          continue;
      }

      dbProperty* minV = dbProperty::find(prop, "MIN");
      dbProperty* maxV = dbProperty::find(prop, "MAX");

      if (minV && maxV) {
        fprintf(_out, "RANGE ");
        writePropValue(minV);
        writePropValue(maxV);
      }

      dbProperty* value = dbProperty::find(prop, "VALUE");

      if (value) {
        writePropValue(value);
      }

      fprintf(_out, ";\n");
    }
  }

  fprintf(_out, "END PROPERTYDEFINITIONS\n");
}

void defout_impl::writePropValue(dbProperty* prop)
{
  switch (prop->getType()) {
    case dbProperty::STRING_PROP: {
      dbStringProperty* p = (dbStringProperty*) prop;
      std::string v = p->getValue();
      fprintf(_out, "\"%s\" ", v.c_str());
      break;
    }

    case dbProperty::INT_PROP: {
      dbIntProperty* p = (dbIntProperty*) prop;
      int v = p->getValue();
      fprintf(_out, "%d ", v);
      break;
    }

    case dbProperty::DOUBLE_PROP: {
      dbDoubleProperty* p = (dbDoubleProperty*) prop;
      double v = p->getValue();
      fprintf(_out, "%G ", v);
    }

    default:
      break;
  }
}

void defout_impl::writeProperties(dbObject* object)
{
  dbSet<dbProperty> props = dbProperty::getProperties(object);
  dbSet<dbProperty>::iterator itr;
  int cnt = 0;

  for (itr = props.begin(); itr != props.end(); ++itr) {
    if (cnt && ((cnt & 3) == 0)) {
      fprintf(_out, "\n    ");
    }

    dbProperty* prop = *itr;
    std::string name = prop->getName();
    fprintf(_out, "%s ", name.c_str());
    writePropValue(prop);
  }
}

bool defout_impl::hasProperties(dbObject* object, ObjType type)
{
  dbSet<dbProperty> props = dbProperty::getProperties(object);
  dbSet<dbProperty>::iterator itr;

  for (itr = props.begin(); itr != props.end(); ++itr) {
    dbProperty* prop = *itr;
    std::string name = prop->getName();

    if (_prop_defs[type].find(name) != _prop_defs[type].end()) {
      return true;
    }
  }

  return false;
}

void defout_impl::writePinProperties(dbBlock* block)
{
  uint cnt = 0;

  dbSet<dbBTerm> bterms = block->getBTerms();
  dbSet<dbBTerm>::iterator bitr;

  for (bitr = bterms.begin(); bitr != bterms.end(); ++bitr) {
    if (hasProperties(*bitr, COMPONENTPIN)) {
      ++cnt;
    }
  }

  dbSet<dbITerm> iterms = block->getITerms();
  dbSet<dbITerm>::iterator iitr;

  for (iitr = iterms.begin(); iitr != iterms.end(); ++iitr) {
    if (hasProperties(*iitr, COMPONENTPIN)) {
      ++cnt;
    }
  }

  if (cnt == 0) {
    return;
  }

  fprintf(_out, "PINPROPERTIES %u ;\n", cnt);

  for (bitr = bterms.begin(); bitr != bterms.end(); ++bitr) {
    dbBTerm* bterm = *bitr;

    if (hasProperties(bterm, COMPONENTPIN)) {
      std::string name = bterm->getName();
      fprintf(_out, "  - PIN %s + PROPERTY ", name.c_str());
      writeProperties(bterm);
      fprintf(_out, " ;\n");
    }
  }

  char ttname[max_name_length];
  for (iitr = iterms.begin(); iitr != iterms.end(); ++iitr) {
    dbITerm* iterm = *iitr;

    if (hasProperties(iterm, COMPONENTPIN)) {
      dbInst* inst = iterm->getInst();
      dbMTerm* mterm = iterm->getMTerm();
      std::string iname = inst->getName();
      // std::string mtname = mterm->getName();
      char* mtname = mterm->getName(inst, &ttname[0]);
      fprintf(_out, "  - %s %s + PROPERTY ", iname.c_str(), mtname);
      writeProperties(iterm);
      fprintf(_out, " ;\n");
    }
  }

  fprintf(_out, "END PINPROPERTIES\n");
}

}  // namespace odb
