import { defineFunction } from '@aws-amplify/backend';

export const signalingFunction = defineFunction({
  name: 'signaling-handler',
  entry: './handler.ts'
});

