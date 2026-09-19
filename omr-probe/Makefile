#
# Copyright (C) 2026 OpenMPTCProuter contributors
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=omr-probe
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omr-probe
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Per-carrier active probes (UDP echo RTT/loss, HTTP capacity) for surveys
  DEPENDS:=+ubus +rpcd +jsonfilter +curl
endef

define Package/omr-probe/description
  omr-probed measures every WAN interface directly (SO_BINDTODEVICE /
  curl --interface), independent of the mqvpn scheduler: a UDP echo probe
  per second per interface (RTT, loss, jitter) and a periodic fixed-size
  HTTP download/upload per interface (capacity) against omr-probe-server
  (server/ in this package) on the VPS. Results are JSONL events
  (probe-YYYYMMDD.jsonl and <session>/probe.jsonl) plus a latest-values
  state served as `ubus call probe get`, which omr-survey samples.
endef

define Package/omr-probe/conffiles
/etc/config/omr-probe
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu99 -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/omr-probed \
		$(PKG_BUILD_DIR)/omr-probed.c
endef

define Package/omr-probe/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omr-probed $(1)/usr/sbin/omr-probed
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omr-probe $(1)/etc/config/omr-probe
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omr-probe $(1)/etc/init.d/omr-probe
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/usr/libexec/rpcd/probe $(1)/usr/libexec/rpcd/probe
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/usr/share/rpcd/acl.d/omr-probe.json $(1)/usr/share/rpcd/acl.d/omr-probe.json
endef

$(eval $(call BuildPackage,omr-probe))
