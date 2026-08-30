<!--
SPDX-FileCopyrightText: 2026 Scitrera LLC
SPDX-FileCopyrightText: 2026 Fox Engine Ltd
SPDX-License-Identifier: GPL-2.0-only
-->

# ColdSnap NVIDIA reset plugin

This CRIU plugin restores the bounded NVIDIA descriptor and VMA residue used
by ColdSnap's n580 snapshot driver. It is loaded into CRIU and uses CRIU's
GPL-covered plugin interface, so it is maintained in the CRIU fork and
distributed under GPL-2.0-only rather than as part of the AGPL-licensed
ColdSnap source tree.

Copyright 2026 Scitrera LLC and Fox Engine Ltd.
