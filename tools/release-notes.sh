#!/usr/bin/env bash
# Génère les notes d'une release : un « - » par commit depuis le tag précédent.
# Usage : tools/release-notes.sh vX.Y.Z [fichier-de-sortie]   (stdout par défaut)
#
# - Version publique (vX.Y.Z) : depuis la précédente version publique, donc les commits des
#   tags de test (-dev…) intermédiaires sont inclus.
# - Tag de test (vX.Y.Z-dev…) : depuis le tag précédent, quel qu'il soit.
# - Les merges et les commits contenant « [skip changelog] » sont ignorés.
# L'application lit ces lignes « - … » (ui/js/features/updater.js, fetchNotes) : une ligne = une nouveauté.
set -euo pipefail

tag="${1:?usage: release-notes.sh <tag> [sortie]}"
[[ -n "${2:-}" ]] && exec > "$2"

# Tag précédent = la plus haute version (pas simplement le plus proche : deux tags peuvent
# pointer le même commit) parmi les tags déjà atteints par l'historique, hors le tag lui-même.
pattern='v[0-9]*'
[[ "$tag" != *-* ]] && pattern='v[0-9]*.[0-9]*.[0-9]*'
prev=$(git -c versionsort.suffix=- tag --merged "$tag^" --list "$pattern" --sort=-v:refname 2>/dev/null \
  | { [[ "$tag" == *-* ]] && cat || grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$'; } \
  | { grep -v -x -F "$tag" || true; } | head -n 1 || true)

range="$tag"
[[ -n "$prev" ]] && range="$prev..$tag"

items=$(git log --no-merges --format='%s' "$range" \
  | { grep -v -i -F '[skip changelog]' || true; } \
  | sed -E 's/[[:space:]]+$//; s/\.$//' \
  | awk 'NF && !seen[$0]++ { print "- " $0 }')

{
  if [[ -n "$items" ]]; then echo "$items"; else echo "- Corrections et améliorations diverses"; fi
  if [[ -n "$prev" && -n "${GITHUB_REPOSITORY:-}" ]]; then
    printf '\n**Full Changelog**: https://github.com/%s/compare/%s...%s\n' "$GITHUB_REPOSITORY" "$prev" "$tag"
  fi
}
