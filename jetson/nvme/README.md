# NVMe data volume

The recorder writes sessions to `/data/recordings` on the NVMe (ext4, label
`AIRPOC-DATA`), mounted by a systemd mount unit — not fstab.

Provision once (destructive to the disk):

```
sudo ./setup_nvme.sh /dev/nvme0n1        # add -f to wipe a non-blank disk
```

- The unit is `WantedBy=local-fs.target` (wants, not requires): a missing or
  dead NVMe never blocks boot. The recorder detects the volume by device id
  (`/data` on the rootfs device = absent) and refuses `rec=start` politely.
- `noatime`: no metadata writes on the 117 MB/s read-back paths (replay/offload).

> Pitfall: don't put `/data` in fstab without `nofail` — a failed NVMe would
> drop the Jetson into emergency mode in the field. The mount unit avoids this
> by construction.
