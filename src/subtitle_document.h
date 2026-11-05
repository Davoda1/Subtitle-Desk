#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// All editing and playback times are integer milliseconds.  A cue is active
// in the half-open interval [startMs, endMs).
struct Cue {
    QString id;
    qint64 startMs = 0;
    qint64 endMs = 1000;
    QString text;
    bool reviewed = false;
};

class SubtitleDocument {
public:
    QVector<Cue> cues;
    QString mediaPath;
    QString model;
    QString language = QStringLiteral("auto");
    QString title;

    static QString newCueId();
    // Accepts HH:MM:SS.mmm and MM:SS.mmm, also comma fractions.  Returns -1
    // and sets ok to false for invalid input.  Two-component minutes may
    // exceed 59, as they often do in LRC files.
    static qint64 parseTime(const QString &text, bool *ok = nullptr);
    static QString formatTime(qint64 milliseconds, bool comma = false);

    // Formats: srt, vtt, ass/ssa, lrc, txt, csv, json/project.
    // Import supports srt, vtt, ass/ssa, lrc, and json/project.  Import and load
    // are transactional: failure leaves the complete existing document intact.
    bool importText(const QString &text, const QString &format, QString *error = nullptr);
    bool load(const QString &filePath, QString *error = nullptr);
    QString exportText(const QString &format, QString *error = nullptr) const;
    bool exportFile(const QString &filePath, const QString &format, QString *error = nullptr) const;
    bool saveProject(const QString &filePath, QString *error = nullptr) const;

    // Invalid intervals block exports; overlaps, empty text and readability
    // thresholds (20 chars/s, 42 chars/line, 2 lines) are advisory warnings.
    // Export orders a copy chronologically and never mutates edits.
    QStringList validationErrors() const;
    QStringList timingWarnings() const;
    void stableSort();
    void normalizeIds();
    qint64 durationMs() const;
    int cueAt(qint64 milliseconds) const;
    QVector<int> activeCuesAt(qint64 milliseconds) const;

    // Split uses the supplied text cursor and playhead.  A playhead outside
    // the cue falls back to its midpoint.  Both resulting cues are unreviewed.
    bool splitCue(int index, int characterPosition, qint64 splitMs, QString *error = nullptr);
    // Merge a contiguous range in the current list, joining text with a line
    // break.  The merged interval covers all selected cues.
    bool mergeCues(int first, int last, QString *error = nullptr);
    // Shift the supplied indices (all cues when empty) by one common offset.
    // Clamps that offset so no start becomes negative and no time overflows.
    // Returns the actual applied offset, preserving all relative cue timing.
    qint64 shiftCues(qint64 offsetMs, const QVector<int> &indices = {});
};
