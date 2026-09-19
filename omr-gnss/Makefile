#
# Copyright (C) 2026 OpenMPTCProuter contributors
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=omr-gnss
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omr-gnss
  SECTION:=net
  CATEGORY:=Network
  TITLE:=NMEA-over-TCP GNSS receiver with ubus state (survey companion)
  DEPENDS:=+ubus +rpcd +jsonfilter
endef

define Package/omr-gnss/description
  omr-gnssd accepts an NMEA 0183 stream pushed over TCP (default port 8620)
  by a GNSS receiver or phone app, keeps the latest sentence per type with
  CLOCK_REALTIME/CLOCK_MONOTONIC receive stamps, publishes it as
  `ubus call gnss get` (consumed by omr-survey as a third source) and can
  append every sentence with its stamps to a daily raw log. No NMEA
  decoding happens on the router.
endef

define Package/omr-gnss/conffiles
/etc/config/omr-gnss
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu99 -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/omr-gnssd \
		$(PKG_BUILD_DIR)/omr-gnssd.c
endef

define Package/omr-gnss/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omr-gnssd $(1)/usr/sbin/omr-gnssd
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omr-gnss $(1)/etc/config/omr-gnss
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omr-gnss $(1)/etc/init.d/omr-gnss
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/usr/libexec/rpcd/gnss $(1)/usr/libexec/rpcd/gnss
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/usr/share/rpcd/acl.d/omr-gnss.json $(1)/usr/share/rpcd/acl.d/omr-gnss.json
endef

$(eval $(call BuildPackage,omr-gnss))
