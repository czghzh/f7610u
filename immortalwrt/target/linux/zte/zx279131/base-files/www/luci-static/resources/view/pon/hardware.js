// SPDX-License-Identifier: Apache-2.0

'use strict';
'require dom';
'require form';
'require fs';
'require ui';
'require view';

var labels = {
	pon_sn: 'PON 序列号',
	device_sn: '设备序列号',
	pon_mac: 'PON MAC 地址',
	board_mac: '主板 MAC 地址',
	lan_base_mac: 'LAN 基础 MAC',
	vendor: '模块厂商',
	part_number: '模块型号',
	serial: '模块序列号',
	temperature: '模块温度 (°C)',
	voltage: '模块电压 (V)',
	tx_power: '发光功率 (dBm)',
	soc: 'SoC',
	model: '设备型号',
	rx_signal: '下行光信号',
	work_mode: 'PON MAC 工作模式',
	serdes_reset: 'RX 就绪',
	serdes_pll: 'SerDes PLL',
	serdes_cdr: 'SerDes 状态字',
	irq_count: 'PON 中断计数',
	olt_state: 'OLT 状态字节 15',
	olt_grant: 'OLT 状态字节 16'
};

function runIdentity(args) {
	return fs.exec('/usr/libexec/pon-board-identity', args).then(function(result) {
		if (result.code != 0)
			throw new Error(result.stderr || result.stdout || _('Board identity operation failed.'));
		return result.stdout.trim();
	});
}

return view.extend({
	load: function() {
		return runIdentity([ 'list' ]).then(function(output) {
			var layout = JSON.parse(output);
			var data = {};
			var sequence = Promise.resolve();

			Object.keys(layout.targets || {}).forEach(function(target) {
				data[target] = {};
				Object.keys(layout.targets[target].fields || {}).forEach(function(field) {
					sequence = sequence.then(function() {
						return runIdentity([ 'read', target, field ]).then(function(value) {
							data[target][field] = value;
						});
					});
				});
			});

			return sequence.then(function() {
				return { layout: layout, data: data };
			});
		});
	},

	render: function(identity) {
		var m = new form.JSONMap(identity.data, _('Hardware identity'));

		this.layout = identity.layout;
		this.data = identity.data;
		this.original = JSON.parse(JSON.stringify(identity.data));
		/*
		 * Identity on this target is read live from the BOSA EEPROM and the
		 * SoC (see /usr/libexec/pon-board-identity).  Nothing is stored, so
		 * the form is always read-only - do not offer a Save button that
		 * cannot work.
		 */
		m.readonly = true;

		Object.keys(identity.layout.targets || {}).forEach(function(target) {
			var fields = identity.layout.targets[target].fields || {};
			var s = m.section(form.NamedSection, target, 'identity', target);

			Object.keys(fields).forEach(function(field) {
				var o = s.option(form.Value, field, labels[field] || field);

				if (fields[field].kind == 'mac')
					o.datatype = 'macaddr';
				o.rmempty = true;
			});
		});

		return m.render();
	},

	handleSave: function() {
		var map = document.querySelector('.cbi-map');
		var self = this;

		return dom.callClassMethod(map, 'save').then(function() {
			var writes = [];
			var results = [];

			Object.keys(self.layout.targets || {}).forEach(function(target) {
				var args = [ 'write', target ];

				Object.keys(self.layout.targets[target].fields || {}).forEach(function(field) {
					var value = self.data[target][field] || '';

					if (value != self.original[target][field])
						args.push(field + '=' + value);
				});
				if (args.length > 2)
					writes.push(args);
			});

			var sequence = Promise.resolve();
			writes.forEach(function(args) {
				sequence = sequence.then(function() {
					return runIdentity(args).then(function(result) { results.push(result); });
				});
			});
			return sequence.then(function() {
				if (writes.length)
					ui.addNotification(null, E('p', [
						_('Hardware identity written. Reboot the device to use the new values.'),
						E('br'), results.join('; ')
					]), 'info');
				self.original = JSON.parse(JSON.stringify(self.data));
			}).catch(function(error) {
				ui.addNotification(null, E('p', [
					_('Hardware identity write failed: %s').format(error.message)
				]), 'danger');
			});
		});
	},

	handleSaveApply: null
});
