export function Greeting(props) {
  return <text class="hi">hello {props.name}</text>;
}

export function App() {
  return (
    <div class="app">
      <Greeting name="ada" />
    </div>
  );
}
