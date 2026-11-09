// Undo and redo for the whole project (DECISIONS D-54).
//
// A step holds the document before and after (when more than values changed)
// and every value that changed. Undoing restores the document part by part,
// touching only what differs (a pad whose sample didn't change isn't
// reloaded), then puts back the values: the current ones, except those the
// step changed, which go back to what they were.

#include <algorithm>

#include "project/session.h"

namespace ion {

namespace {

constexpr auto kMergeWindow = std::chrono::milliseconds(1500);

ParamRef refOf(std::size_t i) {
  return {static_cast<uint8_t>(i / ParamStore::kPerScope), static_cast<uint16_t>(i % ParamStore::kPerScope)};
}

}  // namespace

Session::Doc Session::doc() const { return Doc{tracks_, inserts_, cc_, programs_}; }

bool Session::docUnchanged() const {
  return tracks_ == undoDoc_.tracks && inserts_ == undoDoc_.inserts && cc_ == undoDoc_.cc && programs_ == undoDoc_.programs;
}

void Session::editLabel(std::string label) {
  if (editLabel_.empty()) editLabel_ = std::move(label);
}

void Session::clearHistory() {
  undo_.clear();
  redo_.clear();
  undoDoc_ = doc();
  undoValues_ = values_;
  editLabel_.clear();
}

void Session::commitEdit() {
  const bool docChanged = !docUnchanged();
  Step step;
  for (std::size_t i = 0; i < values_.size(); ++i)
    if (values_[i] != undoValues_[i]) step.values.push_back({static_cast<uint32_t>(i), undoValues_[i], values_[i]});
  if (!docChanged && step.values.empty()) {
    editLabel_.clear();
    return;
  }
  step.at = std::chrono::steady_clock::now();
  if (docChanged) {
    step.before = std::move(undoDoc_);
    step.after = doc();
  }

  // Holding ← → on one value is one step, not forty.
  if (!docChanged && step.values.size() == 1 && redo_.empty() && !undo_.empty()) {
    Step& last = undo_.back();
    const ParamInfo* pi = paramInfo(refOf(step.values[0].index));
    if (!last.before && last.values.size() == 1 && last.values[0].index == step.values[0].index && pi &&
        pi->curve != Curve::Toggle && step.at - last.at < kMergeWindow) {
      last.values[0].after = step.values[0].after;
      last.at = step.at;
      if (last.values[0].after == last.values[0].before) undo_.pop_back();
      undoValues_ = values_;
      editLabel_.clear();
      return;
    }
  }

  if (!editLabel_.empty()) {
    step.label = std::move(editLabel_);
  } else if (!step.values.empty()) {
    step.label = paramLabel(refOf(step.values[0].index));
    if (step.values.size() > 1) step.label += " + " + std::to_string(step.values.size() - 1) + " more";
  } else {
    step.label = "edit";
  }
  editLabel_.clear();
  undo_.push_back(std::move(step));
  if (undo_.size() > kMaxSteps) undo_.pop_front();
  redo_.clear();
  undoDoc_ = doc();
  undoValues_ = values_;
}

std::string Session::undoLabel() const {
  if (!docUnchanged() || values_ != undoValues_) return editLabel_.empty() ? "edit" : editLabel_;
  return undo_.empty() ? std::string() : undo_.back().label;
}

std::string Session::redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }

std::string Session::undo() {
  commitEdit();
  if (undo_.empty()) return {};
  Step step = std::move(undo_.back());
  undo_.pop_back();
  applyStep(step, false);
  std::string label = step.label;
  redo_.push_back(std::move(step));
  return label;
}

std::string Session::redo() {
  commitEdit();  // an edit since the undo clears the redo list, as it should
  if (redo_.empty()) return {};
  Step step = std::move(redo_.back());
  redo_.pop_back();
  applyStep(step, true);
  std::string label = step.label;
  undo_.push_back(std::move(step));
  return label;
}

void Session::applyStep(const Step& step, bool forward) {
  // What the values should end up as: as they are now, except the ones this
  // step changed. (Restoring the document may reset some, e.g. a new engine
  // starts at its defaults; they're put back below.)
  Values target = values_;
  for (const ValueChange& v : step.values) target[v.index] = forward ? v.after : v.before;
  if (step.before) restoreDoc(forward ? *step.after : *step.before);
  for (std::size_t i = 0; i < values_.size(); ++i) {
    if (values_[i] == target[i]) continue;
    const ParamRef ref = refOf(i);
    if (paramInfo(ref)) setValue(ref, target[i]);
    else values_[i] = target[i];  // a slot or engine that isn't there: nothing to send
  }
  undoDoc_ = doc();
  undoValues_ = values_;
  editLabel_.clear();
  markDirty();
}

void Session::restoreDoc(const Doc& d) {
  for (int t = 0; t < kTrackCount; ++t) {
    TrackState& tr = tracks_[static_cast<std::size_t>(t)];
    const TrackState& want = d.tracks[static_cast<std::size_t>(t)];
    if (tr == want) continue;
    if (tr.channel != want.channel) setChannel(t, want.channel);
    // A different engine, or an FM track going back to having no patch: a
    // fresh engine, which loads the voice or the pads it's given.
    if (tr.engine != want.engine || (tr.hasVoice && !want.hasVoice)) {
      tr = want;
      setEngine(t, want.engine);
      continue;
    }
    if (want.hasVoice && (tr.voice != want.voice || tr.voiceSource != want.voiceSource || !tr.hasVoice))
      setVoice(t, want.voice, want.voiceSource);
    tr.libraryId = want.libraryId;
    for (int pad = 0; pad < DrumEngine::kPads; ++pad)
      if (tr.pads[static_cast<std::size_t>(pad)] != want.pads[static_cast<std::size_t>(pad)]) {
        tr.pads[static_cast<std::size_t>(pad)] = want.pads[static_cast<std::size_t>(pad)];
        requestPad(t, pad);
      }
    tr.kit = want.kit;
  }
  for (int scope = 0; scope < kScopeCount; ++scope)
    for (int slot = 0; slot < kInsertSlots; ++slot)
      if (insert(scope, slot) != d.inserts[static_cast<std::size_t>(scope)][static_cast<std::size_t>(slot)])
        setInsert(scope, slot, d.inserts[static_cast<std::size_t>(scope)][static_cast<std::size_t>(slot)]);
  for (int i = 0; i < kMaxCcMappings; ++i) {
    if (cc_[static_cast<std::size_t>(i)] == d.cc[static_cast<std::size_t>(i)]) continue;
    cc_[static_cast<std::size_t>(i)] = d.cc[static_cast<std::size_t>(i)];
    Command c;
    c.type = Command::Type::SetCcMapping;
    c.slot = i;
    c.mapping = cc_[static_cast<std::size_t>(i)];
    send(c);
  }
  if (programs_ != d.programs) {
    programs_ = d.programs;
    sendPrograms();
  }
}

}  // namespace ion
