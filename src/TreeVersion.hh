#ifndef FDP_TREEVERSION_HH
#define FDP_TREEVERSION_HH

#include <string>
#include <vector>

namespace fdp {

// Derives the version token that appears in an object path.
//
// Why versions exist at all: XrdPfc never revalidates. Once an object is
// cached it is served until purged for capacity, and Pelican has no
// per-namespace "do not cache" flag. So a re-analysed shot would be served
// stale forever. Putting a token derived from the tree file in the path means
// regenerating the tree changes the object name, old entries simply age out,
// and cached copies of superseded versions stay correct -- they genuinely are
// that version's bytes.
//
// The token comes from a stat of the `.datafile`: no data is read.
class TreeVersion {
public:
    // `search` is a semicolon-delimited list of templates, mirroring MDSplus's
    // own <tree>_path convention. Placeholders:
    //     %T  tree name        %S  shot number        %B  digit-pair bucket
    // e.g. "/archive/codes/%T/%B/%T_%S.datafile;/archive/shots/%T/%B/%T_%S.datafile"
    // First template that stats wins, which is why the order matters when a
    // tree name exists under more than one branch.
    explicit TreeVersion(const std::string &search);

    bool Configured() const { return !templates_.empty(); }

    // Fills `version` with the current token for (tree, shot). Returns false
    // and fills `error` if no template resolves to an existing file.
    bool Current(const std::string &tree, long long shot,
                 std::string &version, std::string &error) const;

    // Exposed for tests: expand one template.
    static std::string Expand(const std::string &tmpl, const std::string &tree,
                              long long shot);

    // The token used when a request names no tree, so there is nothing to
    // version. Matches kNoTree so the grammar stays uniform.
    static const char *const kNoVersion;   // "-"

private:
    std::vector<std::string> templates_;
};

}  // namespace fdp

#endif
