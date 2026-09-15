#
# Copyright (C) 2026 OpenMPTCProuter contributors
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=omr-survey
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omr-survey
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Mobile connectivity survey sampler (raw JSONL telemetry)
  DEPENDS:=+ubus +rpcd +jsonfilter +omr-metrics +mqvpn
endef

define Package/omr-survey/description
  Records, once per second, the unmodified output of
  `ubus call metrics get_all` (omr-tracker per-WAN metrics) and
  `ubus call mqvpn metrics` (mqvpn get_status + list_paths etc.) as one
  JSON line per sample under /tmp/omr-survey/<session>/survey.jsonl,
  stamped with CLOCK_REALTIME and CLOCK_MONOTONIC. Sessions are started,
  inspected and stopped over ubus (`ubus call omr-survey start|status|stop`).
  No normalization happens on the router; see tools/survey_join.py for the
  off-router post-processing that joins mqvpn per-path metrics back to the
  WAN interface.
endef

define Package/omr-survey/conffiles
/etc/config/omr-survey
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu99 -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/omr-surveyd \
		$(PKG_BUILD_DIR)/omr-surveyd.c
endef

define Package/omr-survey/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omr-surveyd $(1)/usr/sbin/omr-surveyd
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omr-survey $(1)/etc/config/omr-survey
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omr-survey $(1)/etc/init.d/omr-survey
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/usr/libexec/rpcd/omr-survey $(1)/usr/libexec/rpcd/omr-survey
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/usr/share/rpcd/acl.d/omr-survey.json $(1)/usr/share/rpcd/acl.d/omr-survey.json
endef

$(eval $(call BuildPackage,omr-survey))
