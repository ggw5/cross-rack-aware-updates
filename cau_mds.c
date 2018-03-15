#define _GNU_SOURCE


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

#include <net/if.h>
#include <netinet/in.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

#include "common.h"
#include "config.h"

#define UPPBND   9999

int cross_rack_updt_traff;
int num_rcrd_strp;

// sort the data access frequencies in descending order
void quick_sort(int* data, int* index, int start_id, int end_id){

	int left=start_id;
	int right=end_id;

	int p=start_id; //the point

	int guard=data[start_id];
	int guard_id=index[start_id];

	while(left<right){

		while(data[right]<=guard && right>p)
			right--;

		if(data[right]>guard){

			data[p]=data[right];
			index[p]=index[right];
			p=right;
			
			}

		while(data[left]>=guard && left<p)
			left++;

		if(data[left]<guard){

			data[p]=data[left];
			index[p]=index[left];
			p=left;
			
			}
		}

	data[p]=guard;
	index[p]=guard_id;

	if(p-start_id>1)
		quick_sort(data,index,start_id,p-1);

	if(end_id-p>1)
		quick_sort(data,index,p+1,end_id);

}


int find_none_zero_min_array_index(int* array, int num, int exception){

	int i;
	int ret=9999;
	int index=-1;
	int max_updt_freq=-1;

	for(i=0; i<num; i++){

		if(i==exception)
			continue;

		if(array[i]==0)
			continue;

		if(array[i]<ret){
			ret=array[i];
			index=i;
			}

		}

	return index;
}


// send the update command to a node
void* send_cmd_process(void* ptr){

    CMD_DATA tcd = *(CMD_DATA *)ptr;

	printf("Send Commit Stripe-%d CMD to: ", tcd.stripe_id);
	print_amazon_vm_info(tcd.next_ip);

    tcd.port_num=UPDT_PORT; 
	if(if_gateway_open==1) //send the data to the gateway first 
	  send_data(NULL, gateway_ip, UPDT_PORT, NULL, (CMD_DATA*)ptr, CMD_INFO);

	else 
      send_data(NULL, tcd.next_ip, UPDT_PORT, NULL, (CMD_DATA*)ptr, CMD_INFO);

	return NULL;

}

// send the update command to a node
void* send_mvm_data_process(void* ptr){

    TRANSMIT_DATA tcd = *(TRANSMIT_DATA *)ptr;

    tcd.port_num=UPDT_PORT; 
	//send the data to the gateway first 

	if(if_gateway_open==1)
		send_data((TRANSMIT_DATA*)ptr, gateway_ip, UPDT_PORT, NULL, NULL, UPDT_DATA);

	else 
		send_data((TRANSMIT_DATA*)ptr, tcd.next_ip, UPDT_PORT, NULL, NULL, UPDT_DATA);

	return NULL;

}


//@in_chunk_id:  the hot chunk to be move in the hot rack
//@out_chunk_id: the cold chunk to be move from the hot rack
void two_chunk_switch(int in_chunk_id, int in_chnk_node_id, int out_chunk_id, int out_chnk_node_id){

	int i;
	int temp;
	int sum_ack;
	int in_store_order, out_store_order;
	
	//send cmd data for separation to the related two nodes 
	CMD_DATA* mvmn_cmd_mt=(CMD_DATA*)malloc(sizeof(CMD_DATA)*2);

    //for in-chunk 
    mvmn_cmd_mt[0].send_size=sizeof(CMD_DATA);
	mvmn_cmd_mt[0].op_type=CMD_MVMNT;
	mvmn_cmd_mt[0].stripe_id=in_chunk_id/num_chunks_in_stripe;
	mvmn_cmd_mt[0].data_chunk_id=in_chunk_id%num_chunks_in_stripe;
	mvmn_cmd_mt[0].updt_prty_id=-1;
	mvmn_cmd_mt[0].port_num=MVMT_PORT;
	mvmn_cmd_mt[0].prty_delta_app_role=IN_CHNK; // we reuse the item in td structure 
	mvmn_cmd_mt[0].chunk_store_index=locate_store_index(in_chnk_node_id, in_chunk_id);
	memcpy(mvmn_cmd_mt[0].next_ip, node_ip_set[in_chnk_node_id], ip_len);

    //for out-chunk
    mvmn_cmd_mt[1].send_size=sizeof(CMD_DATA);
	mvmn_cmd_mt[1].op_type=CMD_MVMNT;
	mvmn_cmd_mt[1].stripe_id=out_chunk_id/num_chunks_in_stripe;
	mvmn_cmd_mt[1].data_chunk_id=out_chunk_id%num_chunks_in_stripe;
	mvmn_cmd_mt[1].updt_prty_id=-1;
	mvmn_cmd_mt[1].port_num=MVMT_PORT;
	mvmn_cmd_mt[1].prty_delta_app_role=OUT_CHNK;
	mvmn_cmd_mt[1].chunk_store_index=locate_store_index(out_chnk_node_id, out_chunk_id);
	memcpy(mvmn_cmd_mt[1].next_ip, node_ip_set[out_chnk_node_id], ip_len);

	//send the movement cmd
	pthread_t send_cmd_thread[2];
	memset(send_cmd_thread, 0, sizeof(send_cmd_thread));

	//printf("prepare to send data:\n");
	for(i=0; i<2; i++)
		pthread_create(&send_cmd_thread[i], NULL, send_cmd_process, (void *)(mvmn_cmd_mt+i));

	for(i=0; i<2; i++)
		pthread_join(send_cmd_thread[i], NULL);

	//receive data from the two nodes and perform movement
	//printf("prepare to received data:\n");
	para_recv_data(mvmn_cmd_mt[0].stripe_id, 2, MVMT_PORT, 2);

	printf("recv_data_success!\n");

	//send back the data to the two nodes
	TRANSMIT_DATA* mvm_data=(TRANSMIT_DATA*)malloc(sizeof(TRANSMIT_DATA)*2);

	mvm_data[0].send_size=sizeof(TRANSMIT_DATA);
	mvm_data[0].op_type=DATA_MVMNT;
	mvm_data[0].stripe_id=mvmn_cmd_mt[0].stripe_id;
	mvm_data[0].data_chunk_id=mvmn_cmd_mt[0].data_chunk_id;
	mvm_data[0].updt_prty_id=mvmn_cmd_mt[0].updt_prty_id;
	mvm_data[0].port_num=mvmn_cmd_mt[0].port_num;
	mvm_data[0].prty_delta_app_role=mvmn_cmd_mt[0].prty_delta_app_role; // we reuse the item in td structure 
	mvm_data[0].chunk_store_index=mvmn_cmd_mt[0].chunk_store_index;	

	memcpy(mvm_data[0].next_ip, node_ip_set[out_chnk_node_id], ip_len); //send the hot data to the node which stores the cold chunk in the hot rack
	memcpy(mvm_data[0].buff, in_chunk, chunk_size);

	mvm_data[1].send_size=sizeof(TRANSMIT_DATA);
	mvm_data[1].op_type=DATA_MVMNT;
	mvm_data[1].stripe_id=mvmn_cmd_mt[1].stripe_id;
	mvm_data[1].data_chunk_id=mvmn_cmd_mt[1].data_chunk_id;
	mvm_data[1].updt_prty_id=mvmn_cmd_mt[1].updt_prty_id;
	mvm_data[1].port_num=mvmn_cmd_mt[1].port_num;
	mvm_data[1].prty_delta_app_role=mvmn_cmd_mt[1].prty_delta_app_role; // we reuse the item in td structure 
	mvm_data[1].chunk_store_index=mvmn_cmd_mt[1].chunk_store_index;	
	memcpy(mvm_data[1].next_ip, node_ip_set[in_chnk_node_id], ip_len);
	memcpy(mvm_data[1].buff, out_chunk, chunk_size);

	for(i=0; i<2; i++)
		pthread_create(&send_cmd_thread[i], NULL, send_mvm_data_process, (void *)(mvm_data+i));	
	
	for(i=0; i<2; i++)
		pthread_join(send_cmd_thread[i], NULL);

    memset(mvmt_count, 0, sizeof(int)*data_chunks);
	para_recv_ack(mvm_data[0].stripe_id, 2, MVMT_PORT);

	sum_ack=sum_array(data_chunks, mvmt_count);

	if(sum_ack!=2){

		printf("ERR: recv_mvmt_ack\n");
		exit(1);

		}

	printf("recv_mvm_ack success\n");

	//update chunk_map and chunk_store_order
	temp=global_chunk_map[in_chunk_id];
	global_chunk_map[in_chunk_id]=global_chunk_map[out_chunk_id];
	global_chunk_map[out_chunk_id]=temp;

	in_store_order=locate_store_index(in_chnk_node_id, in_chunk_id);
	chunk_store_order[in_chnk_node_id*max_num_store_chunks+in_store_order]=out_chunk_id;

	out_store_order=locate_store_index(out_chnk_node_id, out_chunk_id);
	chunk_store_order[out_chnk_node_id*max_num_store_chunks+out_store_order]=in_chunk_id;

	//update the prty_log_table
	if(cau_num_rplc>0){

		//exchage their log parity 
		int stripe_id;
		int in_dt_id;
		int out_dt_id;
		
		stripe_id=in_chunk_id/num_chunks_in_stripe;
		in_dt_id=in_chunk_id%num_chunks_in_stripe;
		out_dt_id=out_chunk_id%num_chunks_in_stripe;
		
		temp=prty_log_map[stripe_id*data_chunks+out_dt_id];
		prty_log_map[stripe_id*data_chunks+out_dt_id]=prty_log_map[stripe_id*data_chunks+in_dt_id];
		prty_log_map[stripe_id*data_chunks+in_dt_id]=temp;

		}

	//printf("in_store_order=%d\n, out_store_order=%d\n", in_store_order, out_store_order);
	
	//printf("movement for two chunks finishes!\n");

	free(mvmn_cmd_mt);
	
}


//this function is to sort the data chunks in a stripe, based on their access frequencies
//we assume that the data are separated for each commit 
void hot_cold_separation(int num_rcrd_strp){

  printf("\nHot_cold_separation starts:\n");

  int i;
  int j;
  int temp_rack_id;
  int slct_rack;
  int l,h;

  int search_start;

  int in_chunk_id, in_chnk_nd_id;
  int out_chunk_id, out_chnk_nd_id;
  int stripe_id;
  int chunk_id, node_id, rack_id;
  int prty_rack_id;
  int cddt_rack_id;
  int orig_cmmt_cost, new_cmmt_cost;
  int its_rack_id;

  int temp_stripe[data_chunks];
  int temp_dt_chnk_index[data_chunks];
  int temp_dt_updt_freq_stripe[data_chunks];
  
  int rcd_rack_id[rack_num];
  int rack_prty_num[rack_num];

  for(i=0; i<num_rcrd_strp; i++){

	if(mark_updt_stripes_tab[i*(data_chunks+1)]==-1)
		break;

	for(j=0; j<data_chunks; j++)
		temp_dt_chnk_index[j]=j;

	for(j=0; j<data_chunks; j++){

		if(mark_updt_stripes_tab[i*(data_chunks+1)+j+1]>=0)
			temp_dt_updt_freq_stripe[j]=1;
			

		else 
			temp_dt_updt_freq_stripe[j]=-1;
		
		}	

	//sort the data chunks with their indices
	quick_sort(temp_dt_updt_freq_stripe, temp_dt_chnk_index, 0, data_chunks-1);

	memset(rcd_rack_id, 0, sizeof(int)*rack_num);
	memset(rack_prty_num, 0, sizeof(int)*rack_num);

	//find where the rack that has most updated chunks 
	for(j=0; j<data_chunks; j++){

        //we only consider the chunks that are accessed
		if(temp_dt_updt_freq_stripe[j]==-1)
			break;

		stripe_id=mark_updt_stripes_tab[i*(data_chunks+1)];
		chunk_id=temp_dt_chnk_index[j];
		node_id=global_chunk_map[mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[j]];

		temp_rack_id=get_rack_id(node_id);
		rcd_rack_id[temp_rack_id]++;

		}	

	//record the number of parity chunks in racks
	for(l=0; l<num_chunks_in_stripe-data_chunks; l++){
	
		stripe_id=mark_updt_stripes_tab[i*(data_chunks+1)];
		node_id=global_chunk_map[stripe_id*num_chunks_in_stripe+data_chunks+l];
		prty_rack_id=get_rack_id(node_id); 
	
		rack_prty_num[prty_rack_id]++;
	
		}

	//locate the destine rack id that has the maximum number of update chunks
	slct_rack=find_max_array_index(rcd_rack_id, rack_num);
	cddt_rack_id=find_none_zero_min_array_index(rcd_rack_id, rack_num, slct_rack);

/*
    //if there is only one rack with updated data 
	if(cddt_rack_id==-1){

        //if there are more than two chunks updated in the selected rack, then do not move
		if(rcd_rack_id[slct_rack]>1)
			continue;

        //if the selected rack has parity chunks, then do not move 
		if(rack_prty_num[slct_rack]>0)
			continue;

		int max_prty_rack;
		//we can place the single hot data chunk to the rack where there are most parity chunks 
		max_prty_rack=find_max_array_index(rack_prty_num, rack_num);

        //locate the hot chunk
		for(h=0; h<data_chunks; h++){

			if(temp_dt_updt_freq_stripe[h]==1)
				break;
			
			}

		in_chunk_id=mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[h];

        //find a cold chunk in max_prty_rack
		for(h=0; h<data_chunks; h++){

			if(temp_dt_updt_freq_stripe[h]==1)
				continue;

			out_chunk_id=mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[h];

			temp_rack_id=get_rack_id(global_chunk_map[out_chunk_id]);

			if(temp_rack_id==max_prty_rack){

				in_chnk_nd_id=global_chunk_map[in_chunk_id];
				out_chnk_nd_id=global_chunk_map[out_chunk_id];

				//printf("selected_out_chunk=%d, selected_out_node=%d\n", out_chunk_id, out_chnk_nd_id);
				two_chunk_switch(in_chunk_id, in_chnk_nd_id, out_chunk_id, out_chnk_nd_id);
				cross_rack_updt_traff+=2;

				break;

				}

			}

		continue;

		}

*/
	//perform separation for the racks with max and min number of update chunks 
	if(cddt_rack_id!=slct_rack){

        // we prefer the two racks that can group all their stored hot data chunks within a rack
		if(rcd_rack_id[cddt_rack_id]+rcd_rack_id[slct_rack]>node_num_per_rack-rack_prty_num[slct_rack])
			continue;

		orig_cmmt_cost=0;
		new_cmmt_cost=0;

		//the cost of committing the hot data chunks in the rack temp_rack_id before movement
		for(h=0; h<rack_num; h++){

			if(h==cddt_rack_id)
				continue;

			if(rcd_rack_id[cddt_rack_id]<rack_prty_num[h])
					orig_cmmt_cost+=rcd_rack_id[cddt_rack_id];

			else 
				orig_cmmt_cost+=rack_prty_num[h];

			}

		//the cost of committing the hot data chunks in the rack slct_rack_id before movement
		for(h=0; h<rack_num; h++){

			if(h==slct_rack)
				continue;

			if(rcd_rack_id[slct_rack]<rack_prty_num[h])
				orig_cmmt_cost+=rcd_rack_id[slct_rack];

			else 
				orig_cmmt_cost+=rack_prty_num[h];

			}

        //the cost after movement
		for(h=0; h<rack_num; h++){

			if(h==slct_rack)
				continue;

			if(rcd_rack_id[slct_rack]+rcd_rack_id[cddt_rack_id]<rack_prty_num[h])
				new_cmmt_cost+=rcd_rack_id[slct_rack]+rcd_rack_id[cddt_rack_id];

			else 
				new_cmmt_cost+=rack_prty_num[h];

			}

		if(new_cmmt_cost > orig_cmmt_cost-2*rcd_rack_id[cddt_rack_id])
			continue;

		//select a cold chunk from this rack and perform switch
		for(j=0; j<data_chunks; j++){

			if(temp_dt_updt_freq_stripe[j]==-1)
				break;

			its_rack_id=get_rack_id(global_chunk_map[mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[j]]);

			if(its_rack_id!=cddt_rack_id)
				continue;
			
			for(h=0; h<data_chunks; h++){

            	//we only move the chunks that are not updated 
				if(temp_dt_updt_freq_stripe[h]==1) 
					continue;

				//printf("temp_index[k]=%d\n",temp_index[k]);
				temp_rack_id=get_rack_id(global_chunk_map[mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[h]]);

				if(temp_rack_id==slct_rack){

					in_chunk_id=mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[j];
					out_chunk_id=mark_updt_stripes_tab[i*(data_chunks+1)]*num_chunks_in_stripe+temp_dt_chnk_index[h];

					out_chnk_nd_id=global_chunk_map[out_chunk_id];
					in_chnk_nd_id=global_chunk_map[in_chunk_id];
					
					two_chunk_switch(in_chunk_id, in_chnk_nd_id, out_chunk_id, out_chnk_nd_id);
					cross_rack_updt_traff+=2;

					break;
					
				}
			  }
		   }
		}
  	}

  //printf("data_separation completes\n");
}


//establish the map of a data chunk to a parity chunk. This parity chunk is used for log a replica of the data chunk, so as to 
//promise one rack failure tolerance
void cau_estbh_log_map(){

	//printf("enter cau_estbh_log_map:\n");

	int i,j; 
	int k,h;
	int r;
	int its_node_id;
	int prty_nd_id;
	int its_rack_id;
	int dt_rack_id;
	int prty_rack_id;
	int min_load;
	int slct_prty_id;
    int expect_rack_id;

	int map_priority[rack_num][rack_num]; //it records the bandwidth among racks, we select the parity rack with maximum bandwidth for the rack stores updated data

	map_priority[0][0]=0;
	map_priority[0][1]=1;
	map_priority[0][2]=3;
	map_priority[0][3]=2;

	map_priority[1][0]=0;
	map_priority[1][1]=1;
	map_priority[1][2]=2;
	map_priority[1][3]=3;

	map_priority[2][0]=0;
	map_priority[2][1]=1;
	map_priority[2][2]=2;
	map_priority[2][3]=3;

	map_priority[3][0]=0;
	map_priority[3][1]=2;
	map_priority[3][2]=1;
	map_priority[3][3]=3;

	memset(prty_log_map, -1, sizeof(int)*stripe_num*data_chunks);

	for(i=0; i<stripe_num; i++){

		for(k=0; k<data_chunks; k++){

			its_node_id=global_chunk_map[i*num_chunks_in_stripe+k];
			its_rack_id=get_rack_id(its_node_id);

			//scan the parity chunks 
			for(h=0; h<rack_num; h++){

				if(h==its_rack_id)
					continue;

				expect_rack_id=map_priority[its_rack_id][h];

				for(j=0; j<num_chunks_in_stripe-data_chunks; j++){

					prty_nd_id=global_chunk_map[i*num_chunks_in_stripe+data_chunks+j];
					prty_rack_id=get_rack_id(prty_nd_id);

					if(prty_rack_id==expect_rack_id){

						prty_log_map[i*data_chunks+k]=data_chunks+j;
						break;

						}
					}

				if(prty_rack_id==expect_rack_id)
					break;

				}
			}
		}


	//write the mapping info into a file 
	FILE *fd; 
	char* filename="parity_log_map";
	
	fd=fopen(filename,"w");
	if(fd==NULL)
		printf("openfile error!\n");

	for(i=0; i<stripe_num; i++){
	 for(j=0; j<data_chunks; j++)
		 fprintf(fd, "%d ", prty_log_map[i*data_chunks+j]);
	
	 fprintf(fd, "\n");
	 }
	
	fclose(fd);

	//printf("finish parity_log_map\n");

}

// this function seeks the node of the first parity chunk in the given rack and stripe
int get_first_prty_nd_id(int prty_rack_id, int stripe_id){

  int i;
  int global_chunk_id;
  int node_id;
  int rack_id;

  for(i=data_chunks; i<num_chunks_in_stripe; i++){

	global_chunk_id=stripe_id*num_chunks_in_stripe+i;
	node_id=global_chunk_map[global_chunk_id];
	rack_id=get_rack_id(node_id);

	if(rack_id==prty_rack_id) 
		break;
	
  	}

  return node_id;

}

void cau_commit(int num_rcrd_strp){

   printf("++++parity commit starts:+++++\n"); 
   printf("num_rcrd_strp=%d\n", num_rcrd_strp);
   
   int index;
   int i,j,k; 
   int prty_node_id;
   int global_chunk_id;
   int prty_rack_id;
   int updt_node_id;
   int updt_rack_id;
   int updt_stripe_id;
   int delta_num;
   int count;

   int node_id, rack_id;
   int h;
   int updt_chunk_num, prty_num;
   int prty_intl_nd;

   int temp_global_chunk_id;
   int temp_prty_node_id;
   int temp_prty_rack_id;
   int dt_global_chunk_id;

   struct timeval be_time, ed_time;

   //determine the rack_id that stores parity chunks
   //it denotes the number of racks that store parity chunks in CAU
   int prty_rack_num; 

   CMD_DATA* tcd_prty=(CMD_DATA*)malloc(sizeof(CMD_DATA)*(num_chunks_in_stripe-data_chunks));
   CMD_DATA* tcd_dt = (CMD_DATA*)malloc(sizeof(CMD_DATA) * data_chunks);

   int* intnl_nds=(int*)malloc(sizeof(int)*rack_num); //record the internal nodes in each rack
   int* updt_chnk_num_racks=(int*)malloc(sizeof(int)*rack_num); // it records the number of updated chunks in each rack

   pthread_t send_cmd_dt_thread[data_chunks];
   pthread_t send_cmd_prty_thread[num_chunks_in_stripe-data_chunks];

   //printf("mark_updt_stripes_tab:\n");
   //print_array(num_rcrd_strp, data_chunks+1, mark_updt_stripes_tab);
  
   int rack_has_prty[rack_num]; //count the distribution of parity chunks
   
   //commit the deltas stripe by stripe 
   for(i=0; i<num_rcrd_strp; i++){

	 printf("i=%d, num_rcrd_strp=%d\n", i, num_rcrd_strp);
	 
	 updt_stripe_id=mark_updt_stripes_tab[i*(data_chunks+1)];
	 //printf("\ncommit_stripe=%d\n", updt_stripe_id);

	 printf("update_chunks:\n");
	 for(j=0; j<data_chunks; j++)
	 	printf("%d ", mark_updt_stripes_tab[i*(data_chunks+1)+j+1]);
	 printf("\n");

	 printf("%d-th chunk_map:\n", updt_stripe_id);
	 for(j=0; j<num_chunks_in_stripe; j++)
	 	printf("%d ", global_chunk_map[updt_stripe_id*num_chunks_in_stripe+j]);
	 printf("\n");

	 memset(rack_has_prty, 0, sizeof(int)*rack_num);

	 //determine the value of prty_rack_num
	 for(j=0; j<num_chunks_in_stripe-data_chunks; j++){

		global_chunk_id=updt_stripe_id*num_chunks_in_stripe+data_chunks+j;
		prty_node_id=global_chunk_map[global_chunk_id];
		prty_rack_id=get_rack_id(prty_node_id);

		rack_has_prty[prty_rack_id]++;
		
	 	}

	 prty_rack_num=0;
	 for(j=0; j<rack_num; j++)
	 	if(rack_has_prty[j]>=1)
			prty_rack_num++;

	  printf("rack_has_prty:\n");
	  print_array(1, rack_num, rack_has_prty);

	  printf("prty_rack_num=%d\n", prty_rack_num);

	  int* prty_rack_array=(int*)malloc(sizeof(int)*prty_rack_num); // it records the rack_id that stores parity chunks
	  int* prty_num_in_racks=(int*)malloc(sizeof(int)*prty_rack_num); // it records the num of parity chunks in each parity rack
	  int* recv_delta_num=(int*)malloc(sizeof(int)*prty_rack_num); //it records the number of deltas received by the parity chunk in a rack
	  int* first_prty_node_array=(int*)malloc(sizeof(int)*prty_rack_num);
	  int* commit_approach=(int*)malloc(sizeof(int)*rack_num*prty_rack_num); // it records the commit approach from rack-i to rack-j

	  //printf("\nupdate_stripe_id=%d, num_rcrd_strp=%d\n", i, num_rcrd_strp);
	  memset(prty_rack_array, -1, sizeof(int)*prty_rack_num);
	  memset(prty_num_in_racks, 0, sizeof(int)*prty_rack_num);
	  memset(recv_delta_num, 0, sizeof(int)*prty_rack_num);
	  memset(updt_chnk_num_racks, 0, sizeof(int)*rack_num);
	  memset(commit_approach, 0, sizeof(int)*rack_num*prty_rack_num);
	  memset(commit_count, 0, sizeof(int)*(num_chunks_in_stripe-data_chunks));

	  gettimeofday(&be_time, NULL);

	  index=0;

	  //establish the rack_ids that store parity chunks
	  //record the num of parity chunks in each parity rack
	  for(j=0; j<rack_num; j++){

		if(rack_has_prty[j]>=1){

			prty_rack_array[index]=j;
			prty_num_in_racks[index]=rack_has_prty[j];
			index++;

			}
	  	}

	  printf("prty_rack_array:\n");
	  print_array(1, prty_rack_num, prty_rack_array);
	  
	  printf("prty_num_in_racks:\n");
	  print_array(1, prty_rack_num, prty_num_in_racks);

	  //determine the first updated data chunk in the related racks that stores data chunks
	  //notice that it is not the first data node
	  memset(intnl_nds, -1, sizeof(int)*rack_num);
	  
	  for(j=0; j<data_chunks; j++){

		if(mark_updt_stripes_tab[i*(data_chunks+1)+j+1]==-1)
			continue;

		global_chunk_id=updt_stripe_id*num_chunks_in_stripe+j;
		node_id=global_chunk_map[global_chunk_id];
		rack_id=get_rack_id(node_id);

		if(intnl_nds[rack_id]==-1)
			intnl_nds[rack_id]=node_id;
		
	  	}

	  printf("intnl_nds:\n");
	  print_array(1, rack_num, intnl_nds);
	  printf("\n");

	  //store the node that stores the first parity chunk in the given rack and stripe
	  for(j=0; j<prty_rack_num; j++)
	  	first_prty_node_array[j]=get_first_prty_nd_id(prty_rack_array[j], updt_stripe_id);

	  printf("first_prty_node_array:\n");
	  print_array(1, prty_rack_num, first_prty_node_array);	  
	  
	  //scan each updated chunk and record the number of updated chunks in each rack
      for(k=0; k<data_chunks; k++){

		  if(mark_updt_stripes_tab[i*(data_chunks+1)+k+1]==-1)
		  	continue;

		  global_chunk_id=updt_stripe_id*num_chunks_in_stripe+k;
		  updt_node_id=global_chunk_map[global_chunk_id];
		  updt_rack_id=get_rack_id(updt_node_id);
		  updt_chnk_num_racks[updt_rack_id]++;

	  	}

	  printf("updt_chnk_num_racks:\n");
	  print_array(1, rack_num, updt_chnk_num_racks);

	  //determine the number of deltas to be received for each parity chunk 
	  for(j=0; j<prty_rack_num; j++){

		prty_rack_id=prty_rack_array[j];

		for(k=0; k<rack_num; k++){

			if(updt_chnk_num_racks[k]==0)
				continue;

            //for data delta first approach
			if((updt_chnk_num_racks[k] < prty_num_in_racks[j]) && (k!=prty_rack_id)){
				
				recv_delta_num[j] += updt_chnk_num_racks[k];
				commit_approach[k*prty_rack_num+prty_rack_id]=DATA_DELTA_APPR;
				
				}

			else if ((updt_chnk_num_racks[k] >= prty_num_in_racks[j]) && (k!=prty_rack_id)){
				
				recv_delta_num[j] ++; 
				commit_approach[k*prty_rack_num+prty_rack_id]=PARITY_DELTA_APPR;
				
				}

			else if(k==prty_rack_id){
				recv_delta_num[j]+=updt_chnk_num_racks[k];
				commit_approach[k*prty_rack_num+prty_rack_id]=PARITY_DELTA_APPR;
				}

			}
		}

	  printf("recv_delta_num:\n");
	  print_array(1, prty_rack_num, recv_delta_num);	

	  //update cross_rack_updt_traff 
	  for(k=0; k<rack_num; k++){

		 if(updt_chnk_num_racks[k]==0)
		 	continue;

		 for(j=0; j<prty_rack_num; j++){

			if(prty_rack_array[j]==k)
				continue;
			
			if(updt_chnk_num_racks[k]>prty_num_in_racks[j])
				cross_rack_updt_traff+=prty_num_in_racks[j];
			else 
				cross_rack_updt_traff+=updt_chnk_num_racks[k];

		 	}
	  	}

	  //inform the parity chunks first
	  memset(send_cmd_prty_thread, 0, sizeof(send_cmd_prty_thread));
	  
	  for(j=0; j<num_chunks_in_stripe-data_chunks; j++){

        //init structure
        tcd_prty[j].send_size=sizeof(CMD_DATA);
		tcd_prty[j].op_type=DATA_COMMIT;
		tcd_prty[j].prty_delta_app_role=PARITY;
		tcd_prty[j].stripe_id=updt_stripe_id;
		tcd_prty[j].updt_prty_id=j;
		tcd_prty[j].port_num=UPDT_PORT;
		tcd_prty[j].data_chunk_id=-1;
		tcd_prty[j].num_recv_chks_itn=0;
		tcd_prty[j].num_recv_chks_prt=0;
		tcd_prty[j].data_delta_app_prty_role=-1;
		strcpy(tcd_prty[j].from_ip, "");

		global_chunk_id=updt_stripe_id*num_chunks_in_stripe+j+data_chunks;
		prty_node_id=global_chunk_map[global_chunk_id];
		prty_rack_id=get_rack_id(prty_node_id);

        //for the nodes, it can read its ip from sent_ip
		memcpy(tcd_prty[j].sent_ip, node_ip_set[prty_node_id], ip_len);
		memcpy(tcd_prty[j].next_ip, tcd_prty[j].sent_ip, ip_len);

		//printf("tcd_prty[j].sent_ip=%s\n", tcd_prty[j].sent_ip);

        //establish the num of deltas received by the parity chunk
		for(k=0; k<prty_rack_num; k++)
			if(prty_rack_id==prty_rack_array[k])
				break;

		delta_num=recv_delta_num[k];
		tcd_prty[j].num_recv_chks_prt=delta_num; //<------notice this line
		tcd_prty[j].chunk_store_index=locate_store_index(prty_node_id, global_chunk_id);

		//decide which parity chunk should be served as the internal for delta forwarding
		//CAU sets the first parity chunk in each rack to act as the internal node
		if(prty_node_id==first_prty_node_array[k])
			tcd_prty[j].data_delta_app_prty_role=PRTY_INTERNAL;
			
		else 
			tcd_prty[j].data_delta_app_prty_role=PRTY_LEAF;

	  	}

	  //then for each update data chunk, we should define their update approaches and roles in committing for different parity chunks 
	  count=0;
	  
	  for(j=0; j<data_chunks; j++){

		//printf("j=%d, mark_updt_stripes_tab[i*(data_chunks+1)+j+1]=%d\n",j, mark_updt_stripes_tab[i*(data_chunks+1)+j+1]);

		if(mark_updt_stripes_tab[i*(data_chunks+1)+j+1]==-1)
			continue;

		//locate the node_id and rack_id 
		dt_global_chunk_id=updt_stripe_id*num_chunks_in_stripe+j;
		node_id=global_chunk_map[dt_global_chunk_id];
		rack_id=get_rack_id(node_id);

		printf("--data node: ");
		print_amazon_vm_info(node_ip_set[node_id]);
		printf("\n");

		//get the number of update chunks in rack_id
		updt_chunk_num=updt_chnk_num_racks[rack_id];
		//printf("rack_id=%d, updt_chunk_num=%d\n", rack_id, updt_chunk_num);

		if(updt_chunk_num==0){
			printf("ERR: updt_chunk_num==0!\n");
			exit(1);
			}

		//init the common configurations
		tcd_dt[j].send_size=sizeof(CMD_DATA);
		tcd_dt[j].op_type=DATA_COMMIT;
		tcd_dt[j].stripe_id=updt_stripe_id;
		tcd_dt[j].data_chunk_id=j;
		tcd_dt[j].port_num=UPDT_PORT;
		tcd_dt[j].updt_prty_id=-1;
		tcd_dt[j].chunk_store_index=locate_store_index(node_id, dt_global_chunk_id);
		tcd_dt[j].num_recv_chks_itn=0;
		tcd_dt[j].num_recv_chks_prt=0;
		tcd_dt[j].data_delta_app_prty_role=-1;
		tcd_dt[j].prty_delta_app_role=-1;
		strcpy(tcd_dt[j].from_ip, "");
		strcpy(tcd_dt[j].next_ip, "");
		
		memcpy(tcd_dt[j].next_ip, node_ip_set[node_id], ip_len);

		//compare updt_chunk_num to the number of parity chunks in parity rack to determine the commit approach
		for(k=0; k<num_chunks_in_stripe-data_chunks; k++){

			global_chunk_id=updt_stripe_id*num_chunks_in_stripe+data_chunks+k;
			prty_node_id=global_chunk_map[global_chunk_id];
			prty_rack_id=get_rack_id(prty_node_id);

			for(h=0; h<prty_rack_num; h++)
				if(prty_rack_array[h]==prty_rack_id)
					break;

			prty_num=prty_num_in_racks[h];
			tcd_dt[j].updt_prty_nd_id[k]=prty_node_id;

			//printf("prty_rack_id=%d, prty_num=%d\n", prty_rack_id, prty_num);
			//printf("updt_chunk_num=%d, prty_num=%d\n", updt_chunk_num, prty_num);

            //init the configurations in parity-delta-first app and data-delta-first app in the k-th parity chunk's commit
			if((updt_chunk_num >= prty_num) && (rack_id!=prty_rack_id)){

				//printf("Parity-Delta_Approach: stripe_id=%d\n", updt_stripe_id);
				
				tcd_dt[j].commit_app[k]=PARITY_DELTA_APPR;
				
				//define the role of this data chunk in parity-delta-first approach
				//if it is the first data chunk in this rack, then it is defined as an internal node 
				if(intnl_nds[rack_id]==node_id){
					
					tcd_dt[j].prty_delta_app_role=DATA_INTERNAL;
					memcpy(tcd_dt[j].next_dest[k], node_ip_set[prty_node_id], ip_len);

					//decide the number of data deltas received by the internal node 
					tcd_dt[j].num_recv_chks_itn = updt_chunk_num-1; 

					//printf("rack_id=%d, internal_nd_id=%d\n", rack_id, node_id);
					//printf("internal_ip_addr=%s\n", tcd_dt[j].sent_ip);
					//printf("~parity_ip_addr=%s\n", tcd_dt[j].next_dest[k]);
					
					}

				else {
					tcd_dt[j].prty_delta_app_role=DATA_LEAF;
					memcpy(tcd_dt[j].next_dest[k], node_ip_set[intnl_nds[rack_id]], ip_len);

					//printf("rack_id=%d, leaf_nd_id=%d\n", rack_id, node_id);
					//printf("leaf_ip_addr=%s\n", tcd_dt[j].sent_ip);

					}

				}

            //for the data chunks to be committed via data-delta-first approach
			else if ((updt_chunk_num < prty_num) && (rack_id!=prty_rack_id)) {

				tcd_dt[j].commit_app[k]=DATA_DELTA_APPR;
				//the next ip addr is the internal node of parity chunks
				prty_intl_nd=get_first_prty_nd_id(prty_rack_id, updt_stripe_id);
				memcpy(tcd_dt[j].next_dest[k], node_ip_set[prty_intl_nd], ip_len);

				//printf("data-delta-first-approach: tcd_dt[j].next_dest[k]=%s\n", tcd_dt[j].next_dest[k]);
				
				}

            //for the data chunks stored with the parity chunks in the same rack, we directly send the data delta to the parity node
			else if (rack_id==prty_rack_id){

				tcd_dt[j].commit_app[k]=DIRECT_APPR;
				memcpy(tcd_dt[j].next_dest[k], node_ip_set[prty_node_id], ip_len);

				}

			}


		    //send the cmd to the data node
			pthread_create(&send_cmd_dt_thread[count], NULL, send_cmd_process, (void *)(tcd_dt+j));
			count++;

	  	}

	  //send the prty cmd 
	  for(j=0; j<num_chunks_in_stripe-data_chunks; j++)
	  	pthread_create(&send_cmd_prty_thread[j], NULL, send_cmd_process, (void *)(tcd_prty+j));

	  //wait the join the prty_cmd
	  for(j=0; j<num_chunks_in_stripe-data_chunks; j++)
	  	pthread_join(send_cmd_prty_thread[j], NULL);

	  //wait the join of send threads
	  for(j=0; j<count; j++)
		  pthread_join(send_cmd_dt_thread[j], NULL); 

	  gettimeofday(&ed_time, NULL);
	  //printf("info_data_node time=%.2lf\n", ed_time.tv_sec-be_time.tv_sec+(ed_time.tv_usec-be_time.tv_usec)*1.0/1000000);

	  //printf("listen_ack:\n");
	  //receive the acks in parallel 
	  //Notice: we currently assume that the parity_num is no larger than data_num
	  gettimeofday(&be_time, NULL);
	  
	  para_recv_ack(updt_stripe_id, num_chunks_in_stripe-data_chunks, CMMT_PORT);

	  gettimeofday(&ed_time, NULL);
	  //printf("para_recv_ack time=%.2lf\n", ed_time.tv_sec-be_time.tv_sec+(ed_time.tv_usec-be_time.tv_usec)*1.0/1000000);

	  printf("Stripe-%d Commit Completes \n", updt_stripe_id);


	free(prty_rack_array);
	free(prty_num_in_racks);
	free(recv_delta_num);
	free(commit_approach);
	free(first_prty_node_array);


   	}


   free(tcd_prty);
   free(tcd_dt);
   free(intnl_nds);
   free(updt_chnk_num_racks);

}


//read a trace
void cau_md_process_req(UPDT_REQ_DATA* req){

	int local_chunk_id;
	int global_chunk_id; 
	int node_id;
	int j;
	int rack_id;
	int prty_rack_id;
	int stripe_id;
	int chunk_id_in_stripe;
	int log_prty_id, log_prty_rack_id;
	int index;
	int i;
	int its_rack_id, its_prty_nd_id;

	struct timeval ud_bg_tm, ud_ed_tm;
	struct timeval time1_bg, time1_ed;
	struct timeval cm_bg_tm, cm_ed_tm;

    //if the number of logged stripes exceeds the threshold 
    if(num_rcrd_strp>=max_updt_strps){

		gettimeofday(&cm_bg_tm, NULL);
		cau_commit(num_rcrd_strp);
		gettimeofday(&cm_ed_tm, NULL);
		printf("commit_time=%lf\n", cm_ed_tm.tv_sec-cm_bg_tm.tv_sec+(cm_ed_tm.tv_usec-cm_bg_tm.tv_usec)*1.0/1000000);
			
		gettimeofday(&cm_bg_tm, NULL);
		hot_cold_separation(num_rcrd_strp);
		gettimeofday(&cm_ed_tm, NULL);
		printf("separation_time=%lf\n", cm_ed_tm.tv_sec-cm_bg_tm.tv_sec+(cm_ed_tm.tv_usec-cm_bg_tm.tv_usec)*1.0/1000000);

		//re-init the mark_updt_stripes_table
		memset(mark_updt_stripes_tab, -1, sizeof(int)*(max_updt_strps+num_tlrt_strp)*(data_chunks+1));
		num_rcrd_strp=0;
			
        }

	//read the data from req
	local_chunk_id=req->local_chunk_id;
	stripe_id=local_chunk_id/data_chunks;
	chunk_id_in_stripe=local_chunk_id%data_chunks;

	//node info of that chunk 
	global_chunk_id=stripe_id*num_chunks_in_stripe+local_chunk_id%data_chunks;
	node_id=global_chunk_map[global_chunk_id];
	rack_id=get_rack_id(node_id);


	//check if the stripe is recorded
    for(j=0; j<num_rcrd_strp; j++){
	   if(mark_updt_stripes_tab[j*(data_chunks+1)]==stripe_id)
		   break;
    }

	if(j>=num_rcrd_strp){
		mark_updt_stripes_tab[j*(data_chunks+1)]=stripe_id;
		num_rcrd_strp++;
	}

	//record the updated data chunks in the k-th stripe
	mark_updt_stripes_tab[j*(data_chunks+1)+chunk_id_in_stripe+1]++;

   //init the metadata info
   META_INFO* metadata=(META_INFO*)malloc(sizeof(META_INFO));

   metadata->data_chunk_id=local_chunk_id%data_chunks;
   metadata->stripe_id=stripe_id;
   metadata->port_num=UPDT_PORT;

   //fill the data node ip
   memcpy(metadata->next_ip, node_ip_set[node_id], ip_len);

   //fill the parity info
   //tell the data chunk where its corresponding parity chunk for one rack failure is 
   log_prty_id=prty_log_map[local_chunk_id];
   log_prty_rack_id=get_rack_id(global_chunk_map[stripe_id*num_chunks_in_stripe+log_prty_id]);

   if(rack_id!=log_prty_rack_id)
   	cross_rack_updt_traff++;

   //select other cau_num_rplc-1 parity chunk for storing replications
   if(cau_num_rplc > 0){
   	
      memset(metadata->updt_prty_nd_id, -1, sizeof(int)*(num_chunks_in_stripe-data_chunks));
      metadata->updt_prty_nd_id[0]=global_chunk_map[stripe_id*num_chunks_in_stripe+log_prty_id];
   
      index=1;
      for(i=0; i<num_chunks_in_stripe-data_chunks; i++){
  
	     if(index==cau_num_rplc)
	  	   break;

	     if((i+data_chunks)==log_prty_id)
	  	   continue;

	     its_prty_nd_id=global_chunk_map[stripe_id*num_chunks_in_stripe+data_chunks+i];
	     its_rack_id=get_rack_id(its_prty_nd_id);

	     if(its_rack_id!=rack_id)
	  	   cross_rack_updt_traff++;

	     metadata->updt_prty_nd_id[index]=its_prty_nd_id;
	     index++;

   	   }
   	}

	//send the metadata
	send_req(NULL, client_ip, metadata->port_num, metadata, METADATA_INFO);

	free(metadata);
	
}



int main(int argc, char** argv){

    read_chunk_map("chunk_map");
	cau_estbh_log_map();
	get_chunk_store_order();

	//listen the request
	num_rcrd_strp=0;
	memset(mark_updt_stripes_tab, -1, sizeof(int)*(max_updt_strps+num_tlrt_strp)*(data_chunks+1));

	//init socket
	int connfd;
	int server_socket=init_server_socket(UPDT_PORT);

	char* sender_ip;
	int recv_len;
	int read_size;	

	char* recv_buff=(char*)malloc(sizeof(UPDT_REQ_DATA));
	UPDT_REQ_DATA* req=(UPDT_REQ_DATA*)malloc(sizeof(UPDT_REQ_DATA));	

    //init the sender info
    struct sockaddr_in sender_addr;
    socklen_t length=sizeof(sender_addr);

    if(listen(server_socket, 20) == -1){
        printf("Failed to listen.\n");
        exit(1);
    }

	while(1){

		printf("before accept:\n");
		connfd=accept(server_socket, (struct sockaddr*)&sender_addr, &length);
		printf("connfd=%d\n", connfd);
		
		if(connfd<0){

			perror(connfd);
			exit(1);

			}

		sender_ip=inet_ntoa(sender_addr.sin_addr); 

		recv_len=0;
		read_size=0;
		while(recv_len < sizeof(UPDT_REQ_DATA)){

			read_size=read(connfd, recv_buff+recv_len, sizeof(UPDT_REQ_DATA)-recv_len);
			recv_len += read_size;
			
			}	

		memcpy(req, recv_buff, sizeof(UPDT_REQ_DATA));
		cau_md_process_req(req);

		close(connfd);

		}

	free(recv_buff);
	free(req);

    return 0;
}

