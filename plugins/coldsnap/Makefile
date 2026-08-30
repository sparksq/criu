# SPDX-FileCopyrightText: 2026 Scitrera LLC
# SPDX-FileCopyrightText: 2026 Fox Engine Ltd
# SPDX-License-Identifier: GPL-2.0-only

PLUGIN := coldsnap_nvidia_reset_plugin.so

.PHONY: all clean

all: $(PLUGIN)

$(PLUGIN): coldsnap_nvidia_reset_plugin.c
	$(CC) $(CFLAGS) -std=c11 -O2 -fPIC -Wall -Wextra -Werror \
		-Wno-unused-parameter -shared -iquote ../../criu/include $< \
		-Wl,--no-undefined -Wl,-soname,$(PLUGIN) -o $@

clean:
	$(RM) $(PLUGIN)
