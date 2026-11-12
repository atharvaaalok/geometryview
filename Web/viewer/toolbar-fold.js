// Which of the toolbar's tools fold into its More menu, when the toolbar is wider than its space:
// the least used first, a rank at a time, until it fits. Pure, so it can be tested; the Mac's
// Toolbar.swift folds by the same rule (foldPlan there).

// The order tools fold in: lower first; tools of a rank fold together. Tools not named never fold.
export const kFoldRanks = Object.freeze({
  shortcuts: 0, projection: 1, views: 2, picture: 2, fit: 3, open: 4, connect: 4, look: 5, section: 6,
  seeThrough: 7, isolate: 8, ghost: 9, hide: 9, undo: 10, redo: 10, showAll: 11,
});

// The toolbar's measures, as drawn: a card's inset each side, the gap between items, a divider's
// width, the More button's.
export const kToolbarMeasures = Object.freeze({ inset: 10, gap: 8, rule: 1.5, more: 30 });

// `items`, in toolbar order: { key, width, group }, width 0 for a tool not shown at all (another
// platform's, say). Groups are divided by rules, drawn only between groups with something showing;
// More goes last, in the last group. Returns the keys to fold, a Set, empty if all fit.
export function foldPlan(items, available, measures = kToolbarMeasures) {
  const ranks = [...new Set(items.filter((i) => i.width > 0 && i.key in kFoldRanks)
                                 .map((i) => kFoldRanks[i.key]))].sort((a, b) => a - b);
  let folded = new Set();
  for (let n = 0; n <= ranks.length; ++n) {
    folded = new Set(n === 0 ? [] : items.filter((i) => i.key in kFoldRanks && kFoldRanks[i.key] <= ranks[n - 1])
                                         .map((i) => i.key));
    if (widthOf(items, folded, measures) <= available) break;
  }
  return folded;
}

// The toolbar's width with `folded` folded, More included if anything is.
export function widthOf(items, folded, measures = kToolbarMeasures) {
  const shown = items.filter((i) => i.width > 0 && !folded.has(i.key));
  const lastGroup = items.length > 0 ? items[items.length - 1].group : 0;
  if (folded.size > 0) shown.push({ key: "more", width: measures.more, group: lastGroup });
  const groups = new Set(shown.map((i) => i.group)).size;
  const rules = Math.max(0, groups - 1);
  const count = shown.length + rules;
  return 2 * measures.inset + shown.reduce((sum, i) => sum + i.width, 0) + rules * measures.rule +
         Math.max(0, count - 1) * measures.gap;
}

// Whether the rule after group `group` shows: only between groups that both have something showing.
export function ruleShows(items, folded, group) {
  const showing = (g) => items.some((i) => i.group === g && i.width > 0 && !folded.has(i.key)) ||
                         (folded.size > 0 && g === items[items.length - 1].group);
  const groups = [...new Set(items.map((i) => i.group))];
  const at = groups.indexOf(group);
  return at >= 0 && showing(group) && groups.slice(at + 1).some(showing);
}
