export function Card(props) {
  return <div class="card">{props.children}</div>;
}

export function App() {
  return (
    <Card>
      <text class="title">hello</text>
    </Card>
  );
}
