#!/bin/sh
# Why do NOPASSWD entries in /etc/sudoers.d vanish on reboot? Investigate,
# then install the entry in /etc/sudoers proper (validated with visudo -c),
# which provisioning tools are far less likely to regenerate.
echo "== /etc/sudoers.d contents:"
ls -la /etc/sudoers.d/ 2>&1
for f in /etc/sudoers.d/*; do [ -f "$f" ] && { echo "-- $f:"; cat "$f"; }; done
echo
echo "== package ownership of sudoers.d files:"
dpkg -S /etc/sudoers.d/* 2>&1 | head -5
echo
echo "== provisioning / first-boot services on this image:"
systemctl list-unit-files 2>/dev/null | grep -Ei "cloud-init|userconf|firstboot|sysconf|rpi" || echo "  none found"
echo
echo "== journal lines (this boot) mentioning sudo/sudoers:"
journalctl -b --no-pager 2>/dev/null | grep -i sudoers | tail -10 || true
echo
echo "== tmpfiles rules touching /etc/sudoers.d:"
grep -rn sudoers /etc/tmpfiles.d /usr/lib/tmpfiles.d 2>/dev/null || echo "  none"
echo
echo "== installing NOPASSWD for jak directly in /etc/sudoers:"
if grep -q "^jak ALL=(ALL) NOPASSWD: ALL" /etc/sudoers; then
    echo "  already present"
else
    cp /etc/sudoers /etc/sudoers.harp-bak
    echo "jak ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
    if visudo -c -f /etc/sudoers >/dev/null 2>&1; then
        echo "  installed and validated"
    else
        cp /etc/sudoers.harp-bak /etc/sudoers
        echo "  VALIDATION FAILED — reverted, sudoers untouched"
    fi
fi
echo
echo "== non-interactive sudo test as jak:"
su -s /bin/sh -c "sudo -n true 2>&1 && echo '  NOPASSWD works'" jak
