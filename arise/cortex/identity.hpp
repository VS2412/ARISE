#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace arise {

// "Who I am." Persona, voice pointer, soft rules. Stored as a single
// JSON file inside an auto-managed git repo so identity drift is
// reviewable and reversible.
struct IdentityRecord {
    int                                    version = 1;
    std::string                            name = "ARISE";
    std::string                            pronouns = "it/its";
    std::string                            persona_summary;
    std::vector<std::string>               do_list;
    std::vector<std::string>               dont_list;
    std::string                            baseline_mood = "neutral";
    std::optional<std::string>             voice_profile;
    std::chrono::system_clock::time_point  created_at;
    std::chrono::system_clock::time_point  updated_at;
};

class IdentityStore {
public:
    explicit IdentityStore(std::string dir);
    ~IdentityStore();
    IdentityStore(const IdentityStore&) = delete;
    IdentityStore& operator=(const IdentityStore&) = delete;

    // Reads identity.json (or returns a fresh default if absent). Cheap.
    IdentityRecord get() const;

    // Writes the record, preserving created_at if a file already exists,
    // and commits to the local git repo if git is on PATH.
    void set(IdentityRecord rec, const std::string& commit_msg = "update identity");

    std::string dir()      const { return dir_; }
    std::string filePath() const;

    // True iff git is on PATH (probed once at construction).
    bool gitAvailable() const { return git_ok_; }

private:
    std::string dir_;
    bool        git_ok_ = false;

    void initRepo_();
    void commit_(const std::string& msg);
};

} // namespace arise
