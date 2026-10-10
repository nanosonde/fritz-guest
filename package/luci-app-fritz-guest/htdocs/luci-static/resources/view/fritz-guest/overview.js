'use strict';
'require view';
'require form';
'require uci';
'require rpc';
'require poll';
'require network';
'require tools.widgets as widgets';

var callStatus = rpc.declare({
	object: 'fritz-guest',
	method: 'status',
	params: [],
	reject: true
});

function statusRows(status) {
	if (!status || status.schema_version !== 1)
		return [[_('Status'), _('Unavailable')]];
	return [
		[_('State'), status.state],
		[_('Reason'), status.reason],
		[_('Last error'), status.last_error],
		[_('Reload error'), status.reload_error],
		[_('Local address'), status.local_address],
		[_('Server'), status.server],
		[_('Active bridge'), status.bridge],
		[_('Tunnel device'), status.device],
		[_('Local control ID'), status.control_id],
		[_('Peer control ID'), status.peer_control_id],
		[_('Local session ID'), status.session_id],
		[_('Peer session ID'), status.peer_session_id],
		[_('Connected seconds'), status.connected_seconds],
		[_('Reconnects'), status.reconnects],
		[_('Received bytes'), status.rx_bytes],
		[_('Transmitted bytes'), status.tx_bytes]
	];
}

function updateStatus(container, status) {
	var rows = statusRows(status).map(function(row) {
		var value = row[1] == null || row[1] === '' ? '-' : String(row[1]);
		return E('tr', {}, [E('th', { 'scope': 'row' }, row[0]), E('td', {}, value)]);
	});
	container.replaceChildren(E('table', { 'class': 'table' }, rows));
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('fritz-guest'),
			uci.load('network'),
			network.getNetworks(),
			callStatus().catch(function() { return null; })
		]);
	},

	render: function(data) {
		var bridges = [];
		uci.sections('network', 'device', function(section) {
			if (section.type === 'bridge' && section.name)
				bridges.push(section.name);
		});
		bridges = bridges.filter(function(name, index) {
			return bridges.indexOf(name) === index;
		}).sort();

		var map = new form.Map('fritz-guest', _('FRITZ! Guest Network'),
			_('Extends the FRITZ!Box guest network to an existing OpenWrt bridge over an L2TPv3 Ethernet pseudowire.'));
		if (typeof L !== 'undefined' && L.hasViewPermission && L.hasViewPermission() === false)
			map.readonly = true;
		var section = map.section(form.NamedSection, 'guest', 'connection');
		section.addremove = false;
		section.tab('general', _('General'));
		section.tab('advanced', _('Advanced'));
		var option = section.taboption('general', form.Flag, 'enabled', _('Enable'));
		option.default = '0';
		option.rmempty = false;

		option = section.taboption('general', form.Value, 'server', _('FRITZ!Box IPv4 address'));
		option.datatype = 'ip4addr("nomask")';
		option.default = '192.168.178.1';
		option.depends('enabled', '1');
		option.rmempty = false;
		option.retain = true;

		option = section.taboption('general', widgets.NetworkSelect, 'uplink', _('Transport interface'));
		option.description = _('Logical network interface on the home LAN that reaches the FRITZ!Box.');
		option.nocreate = true;
		option.multiple = false;
		option.default = 'lan';
		option.depends('enabled', '1');
		option.rmempty = false;
		option.retain = true;

		option = section.taboption('general', form.ListValue, 'bridge', _('Guest bridge device'));
		option.description = _('Existing bridge device (Network → Devices) that holds the guest SSID or guest ports. It must not carry the home LAN.');
		bridges.forEach(function(name) { option.value(name); });
		var savedBridge = uci.get('fritz-guest', 'guest', 'bridge');
		if (savedBridge && bridges.indexOf(savedBridge) < 0)
			option.value(savedBridge, savedBridge + ' (' + _('unavailable') + ')');
		option.depends('enabled', '1');
		option.rmempty = false;
		option.retain = true;
		option.validate = function(sectionId, value) {
			if (bridges.indexOf(value) < 0)
				return _('Select an existing bridge device.');
			var uplinkOption = section.children.filter(function(child) {
				return child.option === 'uplink';
			})[0];
			var uplink = uplinkOption ? uplinkOption.formvalue(sectionId) : null;
			var transport = data[2].filter(function(interfaceObject) {
				return interfaceObject.getName() === uplink;
			})[0];
			var device = transport ? transport.getDevice() : null;
			if (device && device.getName() === value)
				return _('The guest bridge must differ from the transport device.');
			return true;
		};

		[
			['mtu', _('Guest MTU'), '1500', 'and(uinteger,range(1280,1500))'],
			['hello_interval', _('Idle keepalive interval (seconds)'), '30', 'and(uinteger,range(10,300))'],
			['retry_max', _('Maximum retransmissions'), '5', 'and(uinteger,range(1,10))'],
			['reconnect_max', _('Maximum reconnect delay (seconds)'), '60', 'and(uinteger,range(10,300))']
		].forEach(function(setting) {
			var field = section.taboption('advanced', form.Value, setting[0], setting[1]);
			field.default = setting[2];
			field.datatype = setting[3];
			field.depends('enabled', '1');
			field.rmempty = false;
			field.retain = true;
		});

		var statusContainer = E('div');
		updateStatus(statusContainer, data[3]);
		poll.add(function() {
			return callStatus().then(function(status) {
				updateStatus(statusContainer, status);
			}).catch(function() {
				updateStatus(statusContainer, null);
			});
		}, 5);

		return map.render().then(function(renderedMap) {
			return E('div', {}, [renderedMap, E('h3', {}, _('Connection status')), statusContainer]);
		});
	}
});