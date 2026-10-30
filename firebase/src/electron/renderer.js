let selectedBusId = null;

const btnHost = document.getElementById('btnHost');
const btnConnect = document.getElementById('btnConnect');
const btnRefresh = document.getElementById('btnRefresh');
const btnAttach = document.getElementById('btnAttach');
const btnDetach = document.getElementById('btnDetach');
const btnClearLogs = document.getElementById('btnClearLogs');

const roomInput = document.getElementById('roomInput');
const lblStatus = document.getElementById('lblStatus');
const lblClient = document.getElementById('lblClient');
const lblRoom = document.getElementById('lblRoom');
const badgeDc = document.getElementById('badgeDc');
const deviceList = document.getElementById('deviceList');
const logBox = document.getElementById('logBox');

function appendLog(line) {
  const row = document.createElement('div');
  row.textContent = line;
  logBox.appendChild(row);
  logBox.scrollTop = logBox.scrollHeight;
}

btnClearLogs.addEventListener('click', () => {
  logBox.innerHTML = '';
});

// UI Actions
btnHost.addEventListener('click', async () => {
  const room = roomInput.value.trim();
  if (room) await window.bridge.sendCommand(`SET_ROOM:${room}`);
  await window.bridge.sendCommand('HOST_LOCAL');
});

btnConnect.addEventListener('click', async () => {
  const room = roomInput.value.trim();
  if (room) await window.bridge.sendCommand(`SET_ROOM:${room}`);
  await window.bridge.sendCommand('CONNECT_REMOTE');
});

btnRefresh.addEventListener('click', async () => {
  await window.bridge.sendCommand('REFRESH_DEVICES');
});

btnAttach.addEventListener('click', async () => {
  if (selectedBusId) {
    await window.bridge.sendCommand(`ATTACH:${selectedBusId}`);
  }
});

btnDetach.addEventListener('click', async () => {
  await window.bridge.sendCommand('DETACH');
});

// Bridge Subscriptions
window.bridge.onLog((msg) => appendLog(msg));

window.bridge.onEvent((event) => {
  const { type, payload } = event;

  switch (type) {
    case 'STATE':
      lblStatus.textContent = payload;
      break;

    case 'ERROR':
      lblStatus.textContent = `Error: ${payload}`;
      appendLog(`[ERROR] ${payload}`);
      break;

    case 'ROOM_ID':
      lblRoom.textContent = payload;
      if (!roomInput.value) roomInput.value = payload;
      break;

    case 'CLIENT_ID':
      lblClient.textContent = payload;
      break;

    case 'DATACHANNEL':
      if (payload === 'open') {
        badgeDc.textContent = 'DataChannel: Open';
        badgeDc.className = 'badge badge-online';
      } else {
        badgeDc.textContent = 'DataChannel: Closed';
        badgeDc.className = 'badge badge-offline';
      }
      break;

    case 'CLEAR_REMOTE_DEVICES':
    case 'CLEAR_LOCAL_DEVICES':
      deviceList.innerHTML = '';
      selectedBusId = null;
      btnAttach.disabled = true;
      break;

    case 'REMOTE_DEVICE': {
      const [busid, desc] = payload.split('|');
      addDeviceItem(busid, desc, true);
      break;
    }

    case 'LOCAL_DEVICE': {
      addDeviceItem('', payload, false);
      break;
    }

    case 'BUSY':
      const isBusy = payload === 'true';
      btnAttach.disabled = isBusy || !selectedBusId;
      btnDetach.disabled = isBusy;
      btnHost.disabled = isBusy;
      btnConnect.disabled = isBusy;
      break;
  }
});

function addDeviceItem(busid, desc, selectable) {
  const item = document.createElement('div');
  item.className = 'device-item';
  item.textContent = busid ? `[${busid}] ${desc}` : desc;

  if (selectable) {
    item.addEventListener('click', () => {
      document.querySelectorAll('.device-item').forEach(el => el.classList.remove('selected'));
      item.classList.add('selected');
      selectedBusId = busid;
      btnAttach.disabled = false;
      lblStatus.textContent = `Selected: [${busid}]`;
    });
  }

  deviceList.appendChild(item);
}
