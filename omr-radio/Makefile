#
# Copyright (C) 2026 OpenMPTCProuter contributors
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=omr-radio
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omr-radio
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Phone-side radio telemetry puller (omr-radio Android app) for surveys
  DEPENDS:=+ubus +rpcd +jsonfilter
endef

define Package/omr-radio/description
  omr-radiod connects through each WAN interface to the tethered phone
  (its default gateway) on TCP 8630, where the omr-radio Android app streams
  one JSON line per second with the phone's cell / signal / display-info /
  thermal state. Lines are stored verbatim with receive stamps
  (radio-YYYYMMDD.jsonl and <session>/radio.jsonl); the latest line per
  interface is served as `ubus call radio get` for omr-survey.
endef

define Package/omr-radio/conffiles
/etc/config/omr-radio
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu99 -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/omr-radiod \
		$(PKG_BUILD_DIR)/omr-radiod.c
endef

define Package/omr-radio/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omr-radiod $(1)/usr/sbin/omr-radiod
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omr-radio $(1)/etc/config/omr-radio
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omr-radio $(1)/etc/init.d/omr-radio
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/usr/libexec/rpcd/radio $(1)/usr/libexec/rpcd/radio
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/usr/share/rpcd/acl.d/omr-radio.json $(1)/usr/share/rpcd/acl.d/omr-radio.json
endef

$(eval $(call BuildPackage,omr-radio))
