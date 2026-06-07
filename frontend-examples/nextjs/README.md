# Next.js + NetLeaf

Next.js 全栈示例，使用 NetLeaf 作为 API 后端。

## 快速开始

```bash
cd frontend-examples/nextjs
npm install
npm run dev
```

## API 路由代理

在 `next.config.js` 中配置代理：

```javascript
module.exports = {
  async rewrites() {
    return [
      {
        source: '/api/:path*',
        destination: 'http://localhost:8080/api/:path*'
      }
    ];
  }
};
```

## 服务端获取数据

```javascript
export async function getServerSideProps() {
  const res = await fetch('http://localhost:8080/api');
  const data = await res.json();
  
  return { props: { data } };
}

function Page({ data }) {
  return <div>{data.message}</div>;
}
```
