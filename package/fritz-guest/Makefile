include $(TOPDIR)/rules.mk

PKG_NAME:=fritz-guest
PKG_VERSION:=0.1.0
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/fritz-guest
  SECTION:=net
  CATEGORY:=Network
  TITLE:=FRITZ!Box guest network extension (L2TPv3 Ethernet client)
  DEPENDS:=+libubox +libubus +libuci +libmnl +kmod-l2tp-ip +kmod-l2tp-eth
endef

define Package/fritz-guest/description
  Native daemon that negotiates an RFC 3931 Ethernet pseudowire to a
  FRITZ!Box guest service (fritz.box:guest) over IP protocol 115 and
  attaches the kernel l2tpeth device fritzguest0 to an existing,
  operator-selected guest bridge. Disabled by default.
endef

define Package/fritz-guest/conffiles
/etc/config/fritz-guest
endef

# Sources live in the repository's src/, not in a tarball.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/../../src/*.c $(CURDIR)/../../src/*.h $(CURDIR)/../../src/Makefile $(PKG_BUILD_DIR)/
endef

MAKE_FLAGS += FG_VERSION="$(PKG_VERSION)-r$(PKG_RELEASE)"

define Package/fritz-guest/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz-guestd $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(CURDIR)/files/etc/config/fritz-guest $(1)/etc/config/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(CURDIR)/files/etc/init.d/fritz-guest $(1)/etc/init.d/
endef

$(eval $(call BuildPackage,fritz-guest))
