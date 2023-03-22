import {MessagePaneContainer} from './MessagePaneContainer';
import ThemedLabel from './ThemedLabel';

export default function InvalidHostMessage() {
  return (
    <MessagePaneContainer>
      <ThemedLabel style={{fontSize: 30}}>Invalid Host</ThemedLabel>
      <ThemedLabel style={{fontSize: 18, fontWeight: '400'}}>
        Please ensure your host is setup correctly. You can change your host location by clicking
        the gear shaped icon in the top right.
      </ThemedLabel>
    </MessagePaneContainer>
  );
}
