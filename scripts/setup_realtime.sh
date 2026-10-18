#!/usr/bin/env bash
# Optional: give the current user real-time scheduling permissions for steadier low latency (no longer limited to rtkit's priority 20).
# Needs sudo; log out and back in afterwards. The app works fine without running this script.
set -euo pipefail
USER_NAME="${SUDO_USER:-$USER}"
echo ">> Adding user ${USER_NAME} to the audio group and allowing real-time priority 95 / locked memory"
sudo groupadd -f audio
sudo usermod -aG audio "${USER_NAME}"
sudo tee /etc/security/limits.d/95-guitarstudio-audio.conf > /dev/null << 'LIMITS'
# Guitar Studio: allow the audio group to use real-time scheduling (low-latency audio)
@audio   -  rtprio     95
@audio   -  memlock    unlimited
LIMITS
echo ">> Allowing the audio group to request low CPU wake-up latency (/dev/cpu_dma_latency) to reduce occasional dropouts"
sudo tee /etc/udev/rules.d/95-guitarstudio-cpu-dma-latency.rules > /dev/null << 'UDEV'
KERNEL=="cpu_dma_latency", GROUP="audio", MODE="0660"
UDEV
sudo udevadm control --reload-rules && sudo udevadm trigger --name-match=cpu_dma_latency || true
echo ">> Done. Please log out and log back in."
echo
echo "Optional (less latency jitter): edit /etc/default/grub and add to GRUB_CMDLINE_LINUX_DEFAULT"
echo "    preempt=full threadirqs"
echo "then run sudo update-grub and reboot. (The current kernel defaults to preempt=voluntary)"
