// Web Bluetooth link to a Buddy board.
// The board streams newline-terminated JSON lines on DATA (split to the MTU);
// commands are single JSON objects written to CMD. Both need a passkey bond,
// which Android asks for the first time DATA is read.

export const SVC = '6b1e0001-5d4a-4c1b-9a3e-6c2f0b8d7a10';
const DATA = '6b1e0002-5d4a-4c1b-9a3e-6c2f0b8d7a10';
const CMD = '6b1e0003-5d4a-4c1b-9a3e-6c2f0b8d7a10';

export const supported = () => !!(navigator.bluetooth && navigator.bluetooth.requestDevice);

export class BuddyLink extends EventTarget {
  device = null;
  state = 'off'; // off | connecting | pairing | on | reconnecting
  #data = null;
  #cmd = null;
  #buf = '';
  #decoder = new TextDecoder();
  #writes = Promise.resolve();
  #manualClose = false;
  #retry = 0;
  #retryTimer = null;

  async connect() {
    this.#manualClose = false;
    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: 'Buddy-' }],
      optionalServices: [SVC],
    });
    this.device.addEventListener('gattserverdisconnected', () => this.#onDisconnect());
    await this.#open();
  }

  disconnect() {
    this.#manualClose = true;
    clearTimeout(this.#retryTimer);
    this.#retryTimer = null;
    if (this.device?.gatt.connected) this.device.gatt.disconnect();
    this.#setState('off');
  }

  send(obj) {
    const bytes = new TextEncoder().encode(JSON.stringify(obj));
    // GATT operations can't overlap; chain them
    this.#writes = this.#writes.then(() => this.#cmd?.writeValueWithResponse(bytes)).catch((e) => {
      this.dispatchEvent(new CustomEvent('error', { detail: e }));
    });
    return this.#writes;
  }

  async #open() {
    this.#setState(this.#retry ? 'reconnecting' : 'connecting');
    const server = await this.device.gatt.connect();
    const svc = await server.getPrimaryService(SVC);
    this.#data = await svc.getCharacteristic(DATA);
    this.#cmd = await svc.getCharacteristic(CMD);
    this.#setState('pairing');
    await this.#data.readValue(); // first time: OS pairing dialog, code shown on the board
    this.#buf = '';
    this.#decoder = new TextDecoder();
    this.#data.addEventListener('characteristicvaluechanged', (e) => this.#onChunk(e.target.value));
    await this.#data.startNotifications();
    this.#retry = 0;
    this.#setState('on');
    await this.send({ c: 'hello' });
  }

  #onChunk(view) {
    this.#buf += this.#decoder.decode(view, { stream: true });
    let i;
    while ((i = this.#buf.indexOf('\n')) >= 0) {
      const line = this.#buf.slice(0, i);
      this.#buf = this.#buf.slice(i + 1);
      if (!line.trim()) continue;
      try {
        this.dispatchEvent(new CustomEvent('line', { detail: JSON.parse(line) }));
      } catch {
        console.warn('Bad line from board', line);
      }
    }
  }

  #onDisconnect() {
    this.#data = this.#cmd = null;
    if (this.#manualClose || this.#retryTimer) return;
    this.#setState('reconnecting');
    const delay = Math.min(15000, 1000 * 2 ** this.#retry++);
    this.#retryTimer = setTimeout(() => {
      this.#retryTimer = null;
      if (this.#manualClose) return;
      this.#open().catch(() => this.#onDisconnect());
    }, delay);
  }

  #setState(s) {
    this.state = s;
    this.dispatchEvent(new CustomEvent('state', { detail: s }));
  }
}
