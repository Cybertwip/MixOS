/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * disks.h -- what is plugged into the USB port, as a list the shell can draw.
 *
 * THIS FILE MOUNTS NOTHING.  The image already has a mounter: udev's
 * 99-mixos-automount.rules fires on any sd* carrying a filesystem,
 * mixos-automount@<kernel>.service is instantiated for it, and
 * /run/j36/bin/mixos-automount mounts it under /media with a uid the login user
 * owns and a ladder of fallbacks for a dirty NTFS volume.  All of that is written
 * by the initramfs and all of it runs whether this dashboard is up or not -- a
 * stick plugged in at the login prompt is mounted, and it is mounted the same way.
 * A second mounter inside mixdash would be a second policy for the same disk, and
 * the two would differ on the day one of them was changed.
 *
 * SO WHAT THIS IS is a reader of the answer.  The mounter drops a state file per
 * volume in /run/mixos/volumes/<kernel name> -- device, mountpoint, fstype, label,
 * readonly -- for exactly this purpose, and that file is the primary source here.
 * /proc/self/mountinfo is the secondary one, and it is not redundant: a card built
 * before the automount payload existed, a disk somebody mounted by hand from the
 * Terminal card, and the window between the mount succeeding and the state file
 * being written are all cases where mountinfo knows and the state directory does
 * not.  Anything under /media or /run/media that mountinfo reports and no state
 * file claims is listed too, with the label taken from the directory name.
 *
 * HOW IT LEARNS THAT SOMETHING CHANGED, and why there is no one-second poll.
 * /proc/self/mountinfo is pollable: the kernel raises POLLERR|POLLPRI on it every
 * time the mount table changes, which is the whole of the notification and costs
 * nothing while nothing is happening.  A QSocketNotifier on that descriptor is the
 * event.  Two things are bolted to it:
 *
 *   - The wake is level-triggered until the file is re-read, so the slot seeks the
 *     descriptor back to zero and reads it out before returning.  Skip that and
 *     the notifier fires again immediately, for ever, which on this board is a
 *     dashboard that never idles.
 *   - The mount lands before the state file is written, so the rescan is deferred
 *     by a short single shot rather than run from the wake.  Otherwise the first
 *     look happens in the window described above and the volume appears with its
 *     directory name, then changes name a moment later.
 *
 * The slow timer alongside them is a backstop and is deliberately slow: it is what
 * covers an unmount performed by something that is not us on a kernel where the
 * POLLPRI wake did not arrive, and reading a few hundred bytes of procfs every few
 * seconds is not a cost worth optimising away.
 *
 * EJECT GOES THROUGH SYSTEMD, not through umount.  The unit is BindsTo= its device
 * unit and its ExecStop is `mixos-automount remove', so `systemctl stop' both
 * unmounts the volume and takes the mount out of systemd's world -- where a bare
 * umount would leave an active unit whose ExecStop runs again at shutdown, over a
 * mount point that may by then be something else.  The direct call to the script
 * and the plain umount are the two rungs below it, for a system where systemd is
 * not the one that mounted this.
 */
#ifndef MIXDASH_DISKS_H
#define MIXDASH_DISKS_H

#include <QObject>
#include <QString>
#include <QVector>

class QSocketNotifier;
class QTimer;

/*
 * One mounted external volume.
 *
 * Value type, copied freely: the list is a dozen entries at the very most and the
 * shell wants a snapshot it can hold across a rebuild rather than a pointer into
 * something that is about to be rescanned.
 */
struct Volume {
    /* The kernel name -- "sda1", never a path.  It is the systemd instance name
     * and therefore the only handle eject() needs. */
    QString kernel;
    QString device;         /* /dev/sda1 */
    QString mountPoint;     /* /media/BACKUP */
    QString fstype;         /* vfat, exfat, ntfs3, ext4 ... */
    QString label;          /* as the filesystem carries it; may be empty */
    bool readOnly = false;

    /* What goes on the card.  The label if the filesystem has one, and the mount
     * point's last component otherwise -- which is what the mounter derived from
     * the label in the first place, so for a labelled disk the two agree. */
    QString name() const;

    /*
     * The card key, and it is derived from the mount point rather than from the
     * kernel name.
     *
     * The grid writes the user's arrangement down as a list of keys, so a key has
     * to survive an unplug and a replug -- and "sda1" does not: plug two sticks in
     * the other order and they trade names.  The mount point does survive, because
     * the mounter builds it from the label and appends a suffix only when a second
     * volume of the same name is actually mounted at the same time.
     */
    QString key() const;
};

class Volumes : public QObject
{
    Q_OBJECT

public:
    static Volumes &instance();

    /* Begin watching.  Idempotent, and safe to call before anything is on screen;
     * the first scan happens inside it, so list() is right by the time the grid is
     * first built. */
    void start();

    const QVector<Volume> &list() const { return m_list; }
    /* Null when there is no such volume -- which is the normal answer a moment
     * after somebody pulled the stick out, so every caller checks. */
    const Volume *byKey(const QString &key) const;

    /*
     * Unmount it and stop anything that would remount it.  True when the volume is
     * gone from the mount table afterwards; `error' says what the last attempt
     * said when it is not.
     *
     * Blocking, in the sliced sense shell.h describes -- a flush of a FAT volume
     * on a slow stick is seconds, and the alternative is a card that says nothing
     * until it silently disappears.  The caller shows a busy overlay.
     */
    bool eject(const QString &key, QString *error);

signals:
    /* The list is different from what it was.  Emitted only on a real difference,
     * so a rebuild of the grid is not triggered by the backstop timer finding
     * everything exactly as it left it. */
    void changed();

private slots:
    void onMountEvent();
    void rescan();

private:
    explicit Volumes(QObject *parent = nullptr);

    QVector<Volume> scan() const;

    QVector<Volume> m_list;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_settle = nullptr;     /* debounce between the wake and the look */
    QTimer *m_backstop = nullptr;
    bool m_started = false;
};

#endif /* MIXDASH_DISKS_H */
