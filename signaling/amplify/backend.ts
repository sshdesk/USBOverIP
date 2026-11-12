import { defineBackend } from '@aws-amplify/backend';
import { RestApi, LambdaIntegration } from 'aws-cdk-lib/aws-apigateway';
import { signalingFunction } from './functions/signaling/resource';

const backend = defineBackend({
  signalingFunction,
});

// Create the backend API deployment stack
const apiStack = backend.createStack('SignalingApiStack');

const restApi = new RestApi(apiStack, 'SignalingRestApi', {
  restApiName: 'SignalingService',
  defaultCorsPreflightOptions: {
    allowOrigins: ['*'],
    allowMethods: ['GET', 'POST', 'OPTIONS', 'DELETE'],
    allowHeaders: ['Content-Type'],
  },
});

const lambdaIntegration = new LambdaIntegration(
  backend.signalingFunction.resources.lambda
);

// Map endpoint routes to the Lambda handler
const joinResource = restApi.root.addResource('join');
joinResource.addMethod('POST', lambdaIntegration);

const messagesResource = restApi.root.addResource('messages');
messagesResource.addMethod('POST', lambdaIntegration);
messagesResource.addMethod('GET', lambdaIntegration);

// === ADD THIS AT THE BOTTOM ===
backend.addOutput({
  custom: {
    signalingUrl: restApi.url,
  },
});

