// SPDX-License-Identifier: GPL-2.0-or-later
'use strict';

async function testLuci(source) {
	var fields = [];
	var polls = [];
	var calls = [];
	var currentStatus = null;
	var section = {
		children: fields,
		tab: function() {},
		taboption: function(tab, type, name) {
			var field = {
				option: name,
				tab: tab,
				choices: [],
				value: function(value) { this.choices.push(value); },
				depends: function() {},
				formvalue: function() { return name === 'uplink' ? 'lan' : null; }
			};
			fields.push(field);
			return field;
		}
	};
	var form = {
		Map: function() {
			this.section = function() { return section; };
			this.render = function() { return Promise.resolve(element('form')); };
		},
		NamedSection: {}, Flag: {}, Value: {}, ListValue: {}
	};
	var configuredDevices = [
		{ name: 'br-guest', type: 'bridge' },
		{ name: 'eth0', type: '8021q' },
		{ name: 'br-lan', type: 'bridge' },
		{ name: 'br-guest', type: 'bridge' }
	];
	var uci = {
		load: function() { return Promise.resolve(); },
		sections: function(config, type, callback) { configuredDevices.forEach(callback); },
		get: function() { return 'br-missing'; }
	};
	var networks = [{ getName: function() { return 'lan'; },
		getDevice: function() { return { getName: function() { return 'br-lan'; } }; } }];
	var rpc = {
		declare: function(options) {
			calls.push(options);
			return function() {
				return currentStatus ? Promise.resolve(currentStatus) : Promise.reject(new Error('unavailable'));
			};
		}
	};
	function element(tag, attributes, children) {
		return {
			tag: tag, attributes: attributes, children: children,
			replaceChildren: function(child) { this.children = child; }
		};
	}
	function assert(condition, message) {
		if (!condition) throw new Error(message);
	}
	var page = new Function('view', 'form', 'uci', 'rpc', 'poll', 'network', 'widgets', '_', 'E', source)(
		{ extend: function(value) { return value; } }, form, uci, rpc,
		{ add: function(callback, interval) { polls.push({ callback: callback, interval: interval }); } },
		{ getNetworks: function() { return Promise.resolve(networks); } },
		{ NetworkSelect: {} }, function(text) { return text; }, element);
	var loaded = await page.load();
	assert(loaded[3] === null, 'Unavailable RPC must not prevent form loading');
	var rendered = await page.render(loaded);
	var bridge = fields.filter(function(field) { return field.option === 'bridge'; })[0];
	assert(JSON.stringify(bridge.choices) === JSON.stringify(['br-guest', 'br-lan', 'br-missing']),
		'Bridge selector must exclude non-bridges, deduplicate and preserve missing saved value');
	assert(bridge.validate('guest', 'br-guest') === true, 'Dedicated guest bridge rejected');
	assert(bridge.validate('guest', 'br-lan') !== true, 'Transport bridge must be rejected');
	assert(bridge.validate('guest', 'eth0') !== true, 'Non-bridge must be rejected');
	assert(bridge.validate('guest', 'br-missing') !== true, 'Unavailable bridge must be rejected');
	assert(fields.length === 8, 'UCI field count does not match contract');
	fields.filter(function(field) { return field.option !== 'enabled'; }).forEach(function(field) {
		assert(field.retain === true, 'Disable would erase setting ' + field.option);
	});
	assert(fields.filter(function(field) { return field.tab === 'advanced'; }).length === 4,
		'Advanced tuning fields are not grouped');
	assert(calls[0].object === 'fritz-guest' && calls[0].method === 'status' && calls[0].reject === true,
		'Status RPC does not match backend contract');
	assert(polls.length === 1 && polls[0].interval === 5, 'Polling contract mismatch');
	var statusNode = rendered.children[2];
	assert(statusNode.children.children[0].children[1].children === 'Unavailable',
		'Unavailable status must not be presented as connected');
	currentStatus = { schema_version: 1, state: 'connected', last_error: '<script>bad</script>',
		rx_bytes: '9007199254740993', tx_bytes: '0' };
	await polls[0].callback();
	var rows = statusNode.children.children;
	assert(rows[0].children[1].children === 'connected', 'Connected state not displayed');
	assert(rows[2].children[1].children === '<script>bad</script>', 'Error must remain text content');
	assert(rows[14].children[1].children === '9007199254740993', 'Counter precision lost');
	return 'LuCI syntax, bridge filtering, unsafe selection, setting retention and status contract checks passed';
}

if (typeof module !== 'undefined' && require.main === module) {
	var fs = require('fs');
	var path = require('path');
	var sourcePath = path.join(__dirname, '../package/luci-app-fritz-guest/htdocs/luci-static/resources/view/fritz-guest/overview.js');
	testLuci(fs.readFileSync(sourcePath, 'utf8')).then(function(result) {
		console.log(result);
	}).catch(function(error) {
		console.error(error);
		process.exitCode = 1;
	});
}