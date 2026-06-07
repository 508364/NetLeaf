// TypeScript 类型定义与客户端库
// 为 NetLeaf API 提供类型安全的访问

export interface NetLeafResponse {
  message: string;
  version: string;
}

export interface User {
  id: number;
  name: string;
}

export class NetLeafClient {
  baseUrl: string;

  constructor(baseUrl = 'http://localhost:8080') {
    this.baseUrl = baseUrl;
  }

  async getApiInfo(): Promise<NetLeafResponse> {
    const res = await fetch(`${this.baseUrl}/api`);
    return await res.json();
  }

  async getUsers(): Promise<User[]> {
    const res = await fetch(`${this.baseUrl}/api/users`);
    return await res.json();
  }

  async createUser(name: string): Promise<User> {
    const res = await fetch(`${this.baseUrl}/api/users`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name })
    });
    return await res.json();
  }

  connectWebSocket(path = '/ws') {
    return new WebSocket(`ws://${new URL(this.baseUrl).host}${path}`);
  }
}

export default NetLeafClient;
