/**
 * Copyright (c) 2017 China Mobile IOT.
 * All rights reserved.
 * Reference:
 *  wakaama - https://github.com/eclipse/wakaama
**/

#include "internals.h"

#ifdef LWM2M_BOOTSTRAP

#define PRV_QUERY_BUFFER_LENGTH 64

static void prv_bootstrapFailed( lwm2m_server_t * bootstrapServer )
{
    LOG( "Entering" );

    bootstrapServer->status = STATE_BS_FAILED;
}

static void prv_handleResponse( lwm2m_server_t * bootstrapServer,
                                coap_packet_t * message )
{
    if ( COAP_204_CHANGED == message->code )
    {
        LOG( "Received ACK/2.04, Bootstrap pending, waiting for DEL/PUT from BS server..." );
        bootstrapServer->status = STATE_BS_PENDING;
    }
    else
    {
        prv_bootstrapFailed( bootstrapServer );
    }
}

static void prv_handleBootstrapReply( lwm2m_transaction_t * transaction,
                                      void * message )
{
    lwm2m_server_t * bootstrapServer = (lwm2m_server_t *)transaction->userData;
    coap_packet_t * coapMessage = (coap_packet_t *)message;

    LOG( "Entering" );

    if ( bootstrapServer->status == STATE_BS_INITIATED )
    {
        if ( NULL != coapMessage && COAP_TYPE_RST != coapMessage->type )
        {
            prv_handleResponse( bootstrapServer, coapMessage );
        }
        else
        {
            prv_bootstrapFailed( bootstrapServer );
        }
    }
}

/* start a device initiated bootstrap */
static void prv_requestBootstrap( lwm2m_context_t * context,
                                  lwm2m_server_t * bootstrapServer )
{
    char query[PRV_QUERY_BUFFER_LENGTH];
    int query_length = 0;
    int res;

    LOG( "Entering" );

    query_length = utils_stringCopy( query, PRV_QUERY_BUFFER_LENGTH, "?ep=" );
    if ( query_length < 0 )
    {
        bootstrapServer->status = STATE_BS_FAILED;
        return;
    }
    res = utils_stringCopy( query + query_length, PRV_QUERY_BUFFER_LENGTH - query_length, context->endpointName );
    if ( res < 0 )
    {
        bootstrapServer->status = STATE_BS_FAILED;
        return;
    }
    query_length += res;

    if ( bootstrapServer->sessionH == NULL )
    {
        bootstrapServer->sessionH = lwm2m_connect_server( bootstrapServer->secObjInstID, context->userData );
    }

    if ( bootstrapServer->sessionH != NULL )
    {
        lwm2m_transaction_t * transaction = NULL;

        LOG( "Bootstrap server connection opened" );

        transaction = transaction_new( COAP_TYPE_CON, COAP_POST, NULL, NULL, context->nextMID++, 4, NULL, ENDPOINT_SERVER, (void *)bootstrapServer );
        if ( transaction == NULL )
        {
            bootstrapServer->status = STATE_BS_FAILED;
            return;
        }

        coap_set_header_uri_path( transaction->message, "/"URI_BOOTSTRAP_SEGMENT );
        coap_set_header_uri_query( transaction->message, query );
        transaction->callback = prv_handleBootstrapReply;
        transaction->userData = (void *)bootstrapServer;
        context->transactionList = (lwm2m_transaction_t *)LWM2M_LIST_ADD( context->transactionList, transaction );
        if ( transaction_send( context, transaction ) == 0 )
        {
            LOG( "CI bootstrap requested to BS server" );
            bootstrapServer->status = STATE_BS_INITIATED;
        }
    }
    else
    {
        LOG( "Connecting bootstrap server failed" );
        bootstrapServer->status = STATE_BS_FAILED;
    }
}

void bootstrap_step( lwm2m_context_t * contextP,
                     uint32_t currentTime,
                     time_t* timeoutP )
{
    lwm2m_server_t * targetP;

    LOG( "entering" );
    targetP = contextP->bootstrapServerList;
    while ( targetP != NULL )
    {
        LOG_ARG( "Initial status: %s", STR_STATUS( targetP->status ) );
        switch ( targetP->status )
        {
            case STATE_DEREGISTERED:
            targetP->registration = currentTime + targetP->lifetime;
            targetP->status = STATE_BS_HOLD_OFF;
            if ( *timeoutP > targetP->lifetime )
            {
                *timeoutP = targetP->lifetime;
            }
            break;

            case STATE_BS_HOLD_OFF:
            if ( targetP->registration <= currentTime )
            {
                prv_requestBootstrap( contextP, targetP );
            }
            else if ( *timeoutP > targetP->registration - currentTime )
            {
                *timeoutP = targetP->registration - currentTime;
            }
            break;

            case STATE_BS_INITIATED:
            /* waiting */
            break;

            case STATE_BS_PENDING:
            /* waiting */
            break;

            case STATE_BS_FINISHED:
            if ( targetP->sessionH != NULL )
            {
                lwm2m_close_connection( targetP->sessionH, contextP->userData );
                targetP->sessionH = NULL;
            }
            break;

            case STATE_BS_FAILED:
            if ( targetP->sessionH != NULL )
            {
                lwm2m_close_connection( targetP->sessionH, contextP->userData );
                targetP->sessionH = NULL;
            }
            break;

            default:
            break;
        }
        LOG_ARG( "Finalal status: %s", STR_STATUS( targetP->status ) );
        targetP = targetP->next;
    }
}

coap_status_t bootstrap_handleFinish( lwm2m_context_t * context,
                                      void * fromSessionH )
{
    lwm2m_server_t * bootstrapServer;

    LOG( "Entering" );
    bootstrapServer = utils_findBootstrapServer( context, fromSessionH );
    if ( bootstrapServer != NULL
         && bootstrapServer->status == STATE_BS_PENDING )
    {
        LOG( "Bootstrap server status changed to STATE_BS_FINISHED" );
        bootstrapServer->status = STATE_BS_FINISHED;
        return COAP_204_CHANGED;
    }

    return COAP_IGNORE;
}

/*
* Reset the bootstrap servers statuses
*
* TODO: handle LWM2M Servers the client is registered to ?
*
*/
void bootstrap_start( lwm2m_context_t * contextP )
{
    lwm2m_server_t * targetP;

    LOG( "Entering" );
    targetP = contextP->bootstrapServerList;
    while ( targetP != NULL )
    {
        targetP->status = STATE_DEREGISTERED;
        if ( targetP->sessionH == NULL )
        {
            targetP->sessionH = lwm2m_connect_server( targetP->secObjInstID, contextP->userData );
        }
        targetP = targetP->next;
    }
}

/*
* Returns STATE_BS_PENDING if at least one bootstrap is still pending
* Returns STATE_BS_FINISHED if at least one bootstrap succeeded and no bootstrap is pending
* Returns STATE_BS_FAILED if all bootstrap failed.
*/
lwm2m_status_t bootstrap_getStatus( lwm2m_context_t * contextP )
{
    lwm2m_server_t * targetP;
    lwm2m_status_t bs_status;

    LOG( "Entering" );
    targetP = contextP->bootstrapServerList;
    bs_status = STATE_BS_FAILED;

    while ( targetP != NULL )
    {
        switch ( targetP->status )
        {
            case STATE_BS_FINISHED:
            if ( bs_status == STATE_BS_FAILED )
            {
                bs_status = STATE_BS_FINISHED;
            }
            break;

            case STATE_BS_HOLD_OFF:
            case STATE_BS_INITIATED:
            case STATE_BS_PENDING:
            bs_status = STATE_BS_PENDING;
            break;

            default:
            break;
        }
        targetP = targetP->next;
    }

    LOG_ARG( "Returned status: %s", STR_STATUS( bs_status ) );

    return bs_status;
}

static coap_status_t prv_checkServerStatus( lwm2m_server_t * serverP )
{
    LOG_ARG( "Initial status: %s", STR_STATUS( serverP->status ) );

    switch ( serverP->status )
    {
        case STATE_BS_HOLD_OFF:
        serverP->status = STATE_BS_PENDING;
        LOG_ARG( "Status changed to: %s", STR_STATUS( serverP->status ) );
        break;

        case STATE_BS_INITIATED:
        /* The ACK was probably lost */
        serverP->status = STATE_BS_PENDING;
        LOG_ARG( "Status changed to: %s", STR_STATUS( serverP->status ) );
        break;

        case STATE_DEREGISTERED:
        /* server initiated bootstrap */
        case STATE_BS_PENDING:
        /* do nothing */
        break;

        case STATE_BS_FINISHED:
        case STATE_BS_FAILED:
        default:
        LOG( "Returning COAP_IGNORE" );
        return COAP_IGNORE;
    }

    return COAP_NO_ERROR;
}

static void prv_tagServer( lwm2m_context_t * contextP,
                           uint16_t id )
{
    lwm2m_server_t * targetP;

    targetP = (lwm2m_server_t *)LWM2M_LIST_FIND( contextP->bootstrapServerList, id );
    if ( targetP == NULL )
    {
        targetP = (lwm2m_server_t *)LWM2M_LIST_FIND( contextP->serverList, id );
    }
    if ( targetP != NULL )
    {
        targetP->dirty = true;
    }
}

static void prv_tagAllServer( lwm2m_context_t * contextP,
                              lwm2m_server_t * serverP )
{
    lwm2m_server_t * targetP;

    targetP = contextP->bootstrapServerList;
    while ( targetP != NULL )
    {
        if ( targetP != serverP )
        {
            targetP->dirty = true;
        }
        targetP = targetP->next;
    }
    targetP = contextP->serverList;
    while ( targetP != NULL )
    {
        targetP->dirty = true;
        targetP = targetP->next;
    }
}

coap_status_t bootstrap_handleCommand( lwm2m_context_t * contextP,
                                       lwm2m_uri_t * uriP,
                                       lwm2m_server_t * serverP,
                                       coap_packet_t * message,
                                       coap_packet_t * response )
{
    coap_status_t result;
    lwm2m_media_type_t format;

    LOG_ARG( "Code: %02X", message->code );
    LOG_URI( uriP );
    format = utils_convertMediaType( message->content_type );

    result = prv_checkServerStatus( serverP );
    if ( result != COAP_NO_ERROR ) return result;

    switch ( message->code )
    {
        case COAP_PUT:
        {
            if ( LWM2M_URI_IS_SET_INSTANCE( uriP ) )
            {
                if ( object_isInstanceNew( contextP, uriP->objectId, uriP->instanceId ) )
                {
                    result = object_create( contextP, uriP, format, message->payload, message->payload_len );
                    if ( COAP_201_CREATED == result )
                    {
                        result = COAP_204_CHANGED;
                    }
                }
                else
                {
                    result = object_write( contextP, uriP, format, message->payload, message->payload_len );
                    if ( uriP->objectId == LWM2M_SECURITY_OBJECT_ID
                         && result == COAP_204_CHANGED )
                    {
                        prv_tagServer( contextP, uriP->instanceId );
                    }
                }
            }
            else
            {
                lwm2m_data_t * dataP = NULL;
                int size = 0;
                int i;

                if ( message->payload_len == 0 || message->payload == 0 )
                {
                    result = COAP_400_BAD_REQUEST;
                }
                else
                {
                    size = lwm2m_data_parse( uriP, message->payload, message->payload_len, format, &dataP );
                    if ( size == 0 )
                    {
                        result = COAP_500_INTERNAL_SERVER_ERROR;
                        break;
                    }

                    for ( i = 0; i < size; i++ )
                    {
                        if ( dataP[i].type == LWM2M_TYPE_OBJECT_INSTANCE )
                        {
                            if ( object_isInstanceNew( contextP, uriP->objectId, dataP[i].id ) )
                            {
                                result = object_createInstance( contextP, uriP, &dataP[i] );
                                if ( COAP_201_CREATED == result )
                                {
                                    result = COAP_204_CHANGED;
                                }
                            }
                            else
                            {
                                result = object_writeInstance( contextP, uriP, &dataP[i] );
                                if ( uriP->objectId == LWM2M_SECURITY_OBJECT_ID
                                     && result == COAP_204_CHANGED )
                                {
                                    prv_tagServer( contextP, dataP[i].id );
                                }
                            }

                            if ( result != COAP_204_CHANGED ) /* Stop object create or write when result is error */
                            {
                                break;
                            }
                        }
                        else
                        {
                            result = COAP_400_BAD_REQUEST;
                        }
                    }
                    lwm2m_data_free( size, dataP );
                }
            }
        }
        break;

        case COAP_DELETE:
        {
            if ( LWM2M_URI_IS_SET_RESOURCE( uriP ) )
            {
                result = COAP_400_BAD_REQUEST;
            }
            else
            {
                result = object_delete( contextP, uriP );
                if ( uriP->objectId == LWM2M_SECURITY_OBJECT_ID
                     && result == COAP_202_DELETED )
                {
                    if ( LWM2M_URI_IS_SET_INSTANCE( uriP ) )
                    {
                        prv_tagServer( contextP, uriP->instanceId );
                    }
                    else
                    {
                        prv_tagAllServer( contextP, NULL );
                    }
                }
            }
        }
        break;

        case COAP_GET:
        case COAP_POST:
        default:
        result = COAP_400_BAD_REQUEST;
        break;
    }

    if ( result == COAP_202_DELETED
         || result == COAP_204_CHANGED )
    {
        if ( serverP->status != STATE_BS_PENDING )
        {
            serverP->status = STATE_BS_PENDING;
            contextP->state = STATE_BOOTSTRAPPING;
        }
    }
    LOG_ARG( "Server status: %s", STR_STATUS( serverP->status ) );

    return result;
}

coap_status_t bootstrap_handleDeleteAll( lwm2m_context_t * contextP,
                                         void * fromSessionH )
{
    lwm2m_server_t * serverP;
    coap_status_t result;
    lwm2m_object_t * objectP;

    LOG( "Entering" );
    serverP = utils_findBootstrapServer( contextP, fromSessionH );
    if ( serverP == NULL ) return COAP_IGNORE;
    result = prv_checkServerStatus( serverP );
    if ( result != COAP_NO_ERROR ) return result;

    result = COAP_202_DELETED;
    for ( objectP = contextP->objectList; objectP != NULL; objectP = objectP->next )
    {
        lwm2m_uri_t uri;

        nbiot_memzero( &uri, sizeof(lwm2m_uri_t) );
        uri.flag = LWM2M_URI_FLAG_OBJECT_ID;
        uri.objectId = objectP->objID;

        if ( objectP->objID == LWM2M_SECURITY_OBJECT_ID )
        {
            lwm2m_list_t * instanceP;

            instanceP = objectP->instanceList;
            while ( NULL != instanceP
                    && result == COAP_202_DELETED )
            {
                if ( instanceP->id == serverP->secObjInstID )
                {
                    instanceP = instanceP->next;
                }
                else
                {
                    uri.flag = LWM2M_URI_FLAG_OBJECT_ID | LWM2M_URI_FLAG_INSTANCE_ID;
                    uri.instanceId = instanceP->id;
                    result = object_delete( contextP, &uri );
                    instanceP = objectP->instanceList;
                }
            }
            if ( result == COAP_202_DELETED )
            {
                prv_tagAllServer( contextP, serverP );
            }
        }
        else
        {
            result = object_delete( contextP, &uri );
            if ( result == COAP_405_METHOD_NOT_ALLOWED )
            {
                /* Fake a successful deletion for static objects like the Device object. */
                result = COAP_202_DELETED;
            }
        }
    }

    return result;
}
#endif