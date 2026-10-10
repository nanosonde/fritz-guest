include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-fritz-guest
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-fritz-guest
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=FRITZ!Box guest network configuration and status
  DEPENDS:=+fritz-guest +luci-base
  PKGARCH:=all
endef

define Package/luci-app-fritz-guest/description
  Configure the fritz-guest client and select an existing guest bridge.
  Display live daemon status using its read-only ubus API.
endef

define Build/Prepare
endef
define Build/Configure
endef
define Build/Compile
endef

define Package/luci-app-fritz-guest/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/fritz-guest
	$(INSTALL_DATA) $(CURDIR)/htdocs/luci-static/resources/view/fritz-guest/overview.js $(1)/www/luci-static/resources/view/fritz-guest/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) $(CURDIR)/root/usr/share/luci/menu.d/luci-app-fritz-guest.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) $(CURDIR)/root/usr/share/rpcd/acl.d/luci-app-fritz-guest.json $(1)/usr/share/rpcd/acl.d/
endef

$(eval $(call BuildPackage,luci-app-fritz-guest))