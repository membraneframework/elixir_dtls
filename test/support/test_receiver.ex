defmodule ElixirDTLS.Support.TestReceiver do
  use GenServer

  alias ElixirDTLS

  defmodule State do
    defstruct parent: nil,
              listen_socket: nil,
              peer_socket: nil,
              dtls_socket: nil,
              dtls_pid: nil,
              dtls_to_peer_pid: nil,
              peer_to_dtls_pid: nil
  end

  # Client API
  def start_link(parent, port) do
    GenServer.start_link(__MODULE__, {parent, port})
  end

  def init_dtls_module(pid, dtls_socket_path) do
    GenServer.call(pid, {:init_dtls_module, dtls_socket_path})
  end

  def run_transmit_process(pid) do
    GenServer.call(pid, :run_transmit_process)
  end

  def accept(pid) do
    GenServer.call(pid, :accept)
  end

  # Server API
  @impl true
  def init({parent, port}) do
    state = init_socket(port)
    state = %State{state | parent: parent}
    {:ok, state}
  end

  defp init_socket(port) do
    {:ok, listen_socket} = :socket.open(:inet, :stream, :tcp)
    addr = %{:family => :inet, :port => port, :addr => {127, 0, 0, 1}}
    {:ok, _port} = :socket.bind(listen_socket, addr)
    :ok = :socket.listen(listen_socket)
    %State{listen_socket: listen_socket}
  end

  @impl true
  def handle_call(:accept, _from, %State{listen_socket: listen_socket} = state) do
    {:ok, socket} = :socket.accept(listen_socket)
    new_state = %State{state | peer_socket: socket}
    {:reply, :ok, new_state}
  end

  @impl true
  def handle_call({:init_dtls_module, dtls_socket_path}, _from, state) do
    {:ok, pid} = ElixirDTLS.start_link(self(), dtls_socket_path, false)
    {:ok, socket} = :socket.open(:local, :stream, :default)
    addr = %{:family => :local, :path => dtls_socket_path}
    :ok = :socket.connect(socket, addr)
    new_state = %State{state | dtls_pid: pid, dtls_socket: socket}
    {:reply, :ok, new_state}
  end

  @impl true
  def handle_call(
        :run_transmit_process,
        _from,
        %State{peer_socket: peer_socket, dtls_socket: dtls_socket} = state
      ) do
    dtls_to_peer_pid =
      spawn(fn ->
        dtls_to_peer(dtls_socket, peer_socket)
      end)

    peer_to_dtls_pid =
      spawn(fn ->
        peer_to_dtls(dtls_socket, peer_socket)
      end)

    new_state = %State{
      state
      | dtls_to_peer_pid: dtls_to_peer_pid,
        peer_to_dtls_pid: peer_to_dtls_pid
    }

    {:reply, :ok, new_state}
  end

  @impl true
  def handle_info(msg, %State{parent: parent} = state) do
    IO.inspect(msg, label: "test_receiver")
    send(parent, msg)
    destroy(state)
    {:noreply, state}
  end

  defp dtls_to_peer(dtls_socket, peer_socket) do
    {:ok, data} = :socket.recv(dtls_socket)
    :socket.send(peer_socket, data)
    dtls_to_peer(dtls_socket, peer_socket)
  end

  defp peer_to_dtls(dtls_socket, peer_socket) do
    {:ok, data} = :socket.recv(peer_socket)
    :socket.send(dtls_socket, data)
    peer_to_dtls(dtls_socket, peer_socket)
  end

  defp destroy(state) do
    %State{
      peer_socket: peer_socket,
      dtls_socket: dtls_socket,
      dtls_pid: dtls_pid,
      dtls_to_peer_pid: dtls_to_peer_pid,
      peer_to_dtls_pid: peer_to_dtls_pid
    } = state

    Process.exit(dtls_pid, :normal)
    Process.exit(dtls_to_peer_pid, :normal)
    Process.exit(peer_to_dtls_pid, :normal)
    :socket.shutdown(peer_socket, :read_write)
    :socket.shutdown(dtls_socket, :read_write)
    :socket.close(peer_socket)
    :socket.close(dtls_socket)
  end
end
